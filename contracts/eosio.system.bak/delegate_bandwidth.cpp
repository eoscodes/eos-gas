//
// Created by 超超 on 2018/9/19.
//

/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */
#include "eosio.system.hpp"

#include <eosiolib/eosio.hpp>
#include <eosiolib/print.hpp>
#include <eosiolib/datastream.hpp>
#include <eosiolib/serialize.hpp>
#include <eosiolib/multi_index.hpp>
#include <eosiolib/privileged.h>
#include <eosiolib/transaction.hpp>

#include <eosio.token/eosio.token.hpp>


#include <cmath>
#include <map>

namespace eosiosystem {
	 using eosio::asset;
	 using eosio::indexed_by;
	 using eosio::const_mem_fun;
	 using eosio::bytes;
	 using eosio::print;
	 using eosio::permission_level;
	 using std::map;
	 using std::pair;

	 static constexpr time refund_delay = 3*24*3600;
	 static constexpr time refund_expiration_time = 3600;

	 /**
	  *  Every user 'from' has a scope/table that uses every receipient 'to' as the primary key.
	  */
	 struct delegated_bandwidth {
		  account_name  from;
		  account_name  to;
		  asset         amount;

		  uint64_t  primary_key()const { return to; }

		  // explicit serialization macro is not necessary, used here only to improve compilation time
		  EOSLIB_SERIALIZE( delegated_bandwidth, (from)(to)(amount) )

	 };

	 struct refund_request {
		  account_name  owner;
		  time          request_time;
		  eosio::asset  amount;

		  uint64_t  primary_key()const { return owner; }

		  // explicit serialization macro is not necessary, used here only to improve compilation time
		  EOSLIB_SERIALIZE( refund_request, (owner)(request_time)(amount) )
	 };

	 /**
	  *  These tables are designed to be constructed in the scope of the relevant user, this
	  *  facilitates simpler API for per-user queries
	  */
//   typedef eosio::multi_index< N(userres), user_resources>      user_resources_table;
	 typedef eosio::multi_index< N(delband), delegated_bandwidth> del_bandwidth_table;
	 typedef eosio::multi_index< N(refunds), refund_request>      refunds_table;


	 void validate_b1_vesting( int64_t stake ) {
		 const int64_t base_time = 1527811200; /// 2018-06-01
		 const int64_t max_claimable = 100'000'000'0000ll;
		 const int64_t claimable = int64_t(max_claimable * double(now()-base_time) / (10*seconds_per_year) );

		 eosio_assert( max_claimable - claimable <= stake, "b1 can only claim their tokens over 10 years" );
	 }

	 void system_contract::changebw( account_name from, account_name receiver, asset quantity, bool transfer )
	 {
		 require_auth( from );
		 eosio_assert( quantity != asset(0), "should stake non-zero amount" );

		 account_name source_stake_from = from;
		 if ( transfer ) {
			 from = receiver;
		 }

		 // update stake delegated from "from" to "receiver"
		 {
			 del_bandwidth_table     del_tbl( _self, from);
			 auto itr = del_tbl.find( receiver );
			 if( itr == del_tbl.end() ) {
				 itr = del_tbl.emplace( from, [&]( auto& dbo ){
					  dbo.from          = from;
					  dbo.to            = receiver;
					  dbo.amount        = quantity;
				 });
			 }
			 else {
				 del_tbl.modify( itr, 0, [&]( auto& dbo ){
					  dbo.amount    += quantity;
				 });
			 }
			 eosio_assert( asset(0) <= itr->amount, "insufficient staked" );
			 if ( itr->amount == asset(0) ) {
				 del_tbl.erase( itr );
			 }
		 } // itr can be invalid, should go out of scope

		 // update totals of "receiver"
//      {
//         user_resources_table   totals_tbl( _self, receiver );
//         auto tot_itr = totals_tbl.find( receiver );
//         if( tot_itr ==  totals_tbl.end() ) {
//            tot_itr = totals_tbl.emplace( from, [&]( auto& tot ) {
//                  tot.owner = receiver;
//                  tot.net_weight    = stake_net_delta;
//                  tot.cpu_weight    = stake_cpu_delta;
//               });
//         } else {
//            totals_tbl.modify( tot_itr, from == receiver ? from : 0, [&]( auto& tot ) {
//                  tot.net_weight    += stake_net_delta;
//                  tot.cpu_weight    += stake_cpu_delta;
//               });
//         }
//         eosio_assert( asset(0) <= tot_itr->net_weight, "insufficient staked total net bandwidth" );
//         eosio_assert( asset(0) <= tot_itr->cpu_weight, "insufficient staked total cpu bandwidth" );
//
//         set_resource_limits( receiver, tot_itr->ram_bytes, tot_itr->net_weight.amount, tot_itr->cpu_weight.amount );
//
//         if ( tot_itr->net_weight == asset(0) && tot_itr->cpu_weight == asset(0)  && tot_itr->ram_bytes == 0 ) {
//            totals_tbl.erase( tot_itr );
//         }
//      } // tot_itr can be invalid, should go out of scope

		 // create refund or update from existing refund
		 if ( N(eosio.stake) != source_stake_from ) { //for eosio both transfer and refund make no sense
			 refunds_table refunds_tbl( _self, from );
			 auto req = refunds_tbl.find( from );

			 //create/update/delete refund
			 auto balance = quantity;
//         auto cpu_balance = stake_cpu_delta;
			 bool need_deferred_trx = false;


			 // net and cpu are same sign by assertions in delegatebw and undelegatebw
			 // redundant assertion also at start of changebw to protect against misuse of changebw
			 bool is_undelegating = balance.amount < 0;
			 bool is_delegating_to_self = (!transfer && from == receiver);

			 if( is_delegating_to_self || is_undelegating ) {
				 if ( req != refunds_tbl.end() ) { //need to update refund
					 refunds_tbl.modify( req, 0, [&]( refund_request& r ) {
						  if ( balance < asset(0) ) {
							  r.request_time = now();
						  }
						  r.amount -= balance;
						  if ( r.amount < asset(0) ) {
							  balance = -r.amount;
							  r.amount = asset(0);
						  } else {
							  balance = asset(0);
						  }
//                  r.cpu_amount -= cpu_balance;
//                  if ( r.cpu_amount < asset(0) ){
//                     cpu_balance = -r.cpu_amount;
//                     r.cpu_amount = asset(0);
//                  } else {
//                     cpu_balance = asset(0);
//                  }
					 });

					 eosio_assert( asset(0) <= req->amount, "negative refund amount" ); //should never happen
//               eosio_assert( asset(0) <= req->cpu_amount, "negative cpu refund amount" ); //should never happen

//               if ( req->net_amount == asset(0) && req->cpu_amount == asset(0) ) {
					 if ( req->amount == asset(0) ) {
						 refunds_tbl.erase( req );
						 need_deferred_trx = false;
					 } else {
						 need_deferred_trx = true;
					 }

				 } else if ( balance < asset(0) ) { //need to create refund
					 refunds_tbl.emplace( from, [&]( refund_request& r ) {
						  r.owner = from;
						  if ( balance < asset(0) ) {
							  r.amount = -balance;
							  balance = asset(0);
						  } // else r.net_amount = 0 by default constructor
//                  if ( cpu_balance < asset(0) ) {
//                     r.cpu_amount = -cpu_balance;
//                     cpu_balance = asset(0);
//                  } // else r.cpu_amount = 0 by default constructor
						  r.request_time = now();
					 });
					 need_deferred_trx = true;
				 } // else stake increase requested with no existing row in refunds_tbl -> nothing to do with refunds_tbl
			 } /// end if is_delegating_to_self || is_undelegating

			 if ( need_deferred_trx ) {
				 eosio::transaction out;
				 out.actions.emplace_back( permission_level{ from, N(active) }, _self, N(refund), from );
				 out.delay_sec = refund_delay;
				 cancel_deferred( from ); // TODO: Remove this line when replacing deferred trxs is fixed
				 out.send( from, from, true );
			 } else {
				 cancel_deferred( from );
			 }

			 auto transfer_amount = balance;
			 if ( asset(0) < transfer_amount ) {
				 INLINE_ACTION_SENDER(eosio::token, transfer)( N(eosio.token), {source_stake_from, N(active)},
																			  { source_stake_from, N(eosio.stake), asset(transfer_amount), std::string("stake bandwidth") } );
			 }
		 }

		 // update voting power
		 {
			 asset total_update = quantity;
			 auto from_voter = _voters.find(from);
			 if( from_voter == _voters.end() ) {
				 from_voter = _voters.emplace( from, [&]( auto& v ) {
					  v.owner  = from;
					  v.staked = total_update.amount;
				 });
			 } else {
				 _voters.modify( from_voter, 0, [&]( auto& v ) {
					  v.staked += total_update.amount;
				 });
			 }
			 eosio_assert( 0 <= from_voter->staked, "stake for voting cannot be negative");
			 if( from == N(b1) ) {
				 validate_b1_vesting( from_voter->staked );
			 }

			 if( from_voter->producers.size() || from_voter->proxy ) {
				 update_votes( from, from_voter->proxy, from_voter->producers, false );
			 }
		 }
	 }

//   void system_contract::delegatebw( account_name from, account_name receiver,
//                                     asset stake_net_quantity,
//                                     asset stake_cpu_quantity, bool transfer )
//   {
//      eosio_assert( stake_cpu_quantity >= asset(0), "must stake a positive amount" );
//      eosio_assert( stake_net_quantity >= asset(0), "must stake a positive amount" );
//      eosio_assert( stake_net_quantity + stake_cpu_quantity > asset(0), "must stake a positive amount" );
//      eosio_assert( !transfer || from != receiver, "cannot use transfer flag if delegating to self" );
//
//      changebw( from, receiver, stake_net_quantity, stake_cpu_quantity, transfer);
//   } // delegatebw

	 void system_contract::delegatebw( account_name from, account_name receiver, asset stake_quantity, bool transfer )
	 {
		 eosio_assert( stake_quantity > asset(0), "must stake a positive amount" );
		 eosio_assert( !transfer || from != receiver, "cannot use transfer flag if delegating to self" );

		 changebw( from, receiver, stake_quantity, transfer);
	 } // delegatebw without resource

	 void system_contract::undelegatebw( account_name from, account_name receiver,
													 asset unstake_quantity )
	 {
		 eosio_assert( asset() < unstake_quantity, "must unstake a positive amount" );
		 eosio_assert( _gstate.total_activated_stake >= min_activated_stake,
							"cannot undelegate bandwidth until the chain is activated (at least 15% of all tokens participate in voting)" );

		 changebw( from, receiver, -unstake_quantity, false);
	 } // undelegatebw


	 void system_contract::refund( const account_name owner ) {
		 require_auth( owner );

		 refunds_table refunds_tbl( _self, owner );
		 auto req = refunds_tbl.find( owner );
		 eosio_assert( req != refunds_tbl.end(), "refund request not found" );
		 eosio_assert( req->request_time + refund_delay <= now(), "refund is not available yet" );
		 // Until now() becomes NOW, the fact that now() is the timestamp of the previous block could in theory
		 // allow people to get their tokens earlier than the 3 day delay if the unstake happened immediately after many
		 // consecutive missed blocks.

		 INLINE_ACTION_SENDER(eosio::token, transfer)( N(eosio.token), {N(eosio.stake),N(active)},
																	  { N(eosio.stake), req->owner, req->amount, std::string("unstake") } );

		 refunds_tbl.erase( req );
	 }


} //namespace eosiosystem
