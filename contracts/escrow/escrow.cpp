#include "escrow.hpp"
#include <eosio/system.hpp>

namespace eosio
{

void escrow::create(const name &issuer,
                    const asset &maximum_supply)
{
  require_auth(get_self());

  auto sym = maximum_supply.symbol;
  check(sym.is_valid(), "invalid symbol name");
  check(maximum_supply.is_valid(), "invalid supply");
  check(maximum_supply.amount > 0, "max-supply must be positive");

  stats statstable(get_self(), sym.code().raw());
  auto existing = statstable.find(sym.code().raw());
  check(existing == statstable.end(), "escrow with symbol already exists");

  statstable.emplace(get_self(), [&](auto &s) {
    s.supply.symbol = maximum_supply.symbol;
    s.max_supply = maximum_supply;
    s.issuer = issuer;
  });
}

void escrow::issue(const name &to, const asset &quantity, const string &memo)
{
  auto sym = quantity.symbol;
  check(sym.is_valid(), "invalid symbol name");
  check(memo.size() <= 256, "memo has more than 256 bytes");

  stats statstable(get_self(), sym.code().raw());
  auto existing = statstable.find(sym.code().raw());
  check(existing != statstable.end(), "escrow with symbol does not exist, create escrow before issue");
  const auto &st = *existing;
  check(to == st.issuer, "escrows can only be issued to issuer account");

  require_auth(st.issuer);
  only_issue(to, quantity, memo);
  add_balance(st.issuer, quantity, st.issuer);
  asset stakequantity = quantity;
  stakequantity.amount = 0;
  add_staking_entries( to, stakequantity);
}

void escrow::changewindow(const name &to, const asset &quantity, const int64_t interval)
{
  auto sym = quantity.symbol;
  check(sym.is_valid(), "invalid symbol name");

  stats statstable(get_self(), sym.code().raw());
  auto existing = statstable.find(sym.code().raw());
  check(existing != statstable.end(), "escrow with symbol does not exist, create escrow before issue");
  const auto &st = *existing;
  check(to == st.issuer, "escrows can only be changed by issuer account");

  require_auth(st.issuer);
  STAKING_INTERVAL = interval;
}

void escrow::only_issue(const name &to, const asset &quantity, const string &memo)
{
  auto sym = quantity.symbol;
  check(sym.is_valid(), "invalid symbol name");

  stats statstable(get_self(), sym.code().raw());
  auto existing = statstable.find(sym.code().raw());
  check(existing != statstable.end(), "escrow with symbol does not exist, create escrow before issue");
  const auto &st = *existing;

  check(quantity.is_valid(), "invalid quantity");
  check(quantity.amount > 0, "must issue positive quantity");

  check(quantity.symbol == st.supply.symbol, "symbol precision mismatch");
  check(quantity.amount <= st.max_supply.amount - st.supply.amount, "quantity exceeds available supply");

  statstable.modify(st, same_payer, [&](auto &s) {
    s.supply += quantity;
  });

}

void escrow::stake(const name &from, const asset &quantity)
{
  require_auth(from);
  auto sym = quantity.symbol.code();
  stats statstable(get_self(), sym.raw());
  const auto &st = statstable.get(sym.raw());

  require_recipient(from);

  check(quantity.is_valid(), "invalid quantity");
  check(quantity.amount > 0, "must transfer positive quantity");
  check(quantity.symbol == st.supply.symbol, "symbol precision mismatch");

  stakerecords stakerecordstable(get_self(), get_first_receiver().value);
  auto iterator = stakerecordstable.find(from.value);
  if (iterator == stakerecordstable.end())
  {
    sub_balance(from, quantity);
    stakerecordstable.emplace(from, [&](auto &row) {
      row.key = from;
      row.stakingamount = quantity;
      row.stakingtime = eosio::current_time_point();
    });
    add_staking_entries( from, quantity);
  }
  else
  {
    //The user is in the table
  }
}

void escrow::release(const name &from)
{
  require_auth(from);
  require_recipient(from);

  stakerecords stakerecordstable(get_self(), get_first_receiver().value);
  auto iterator = stakerecordstable.find(from.value);
  if (iterator != stakerecordstable.end())
  {
    int64_t rewards_earned = 0;
    allstakes all_staking_iterator(get_self(), get_first_receiver().value);
    auto it2 = all_staking_iterator.begin();
    ++it2;
    for( auto it = all_staking_iterator.begin(); it != all_staking_iterator.end(); ++it ) {
      
      if(it2 == all_staking_iterator.end()) {
        if(iterator->stakingtime < it->time) {
          auto delta_time = eosio::current_time_point().time_since_epoch() - it->time.time_since_epoch();
          rewards_earned += (delta_time.count())*(it->reward_multiplier);
        } else {
          auto delta_time = eosio::current_time_point().time_since_epoch() - iterator->stakingtime.time_since_epoch();
          rewards_earned += (delta_time.count())*(it->reward_multiplier);
        }
        break;
      }

      if (it2->time >= iterator->stakingtime && iterator->stakingtime > it->time) {
        auto delta_time = it2->time.time_since_epoch() - iterator->stakingtime.time_since_epoch();
        rewards_earned += (delta_time.count())*(it->reward_multiplier); 
      } else if (it->time > iterator->stakingtime && it2!=all_staking_iterator.end()) {
        auto delta_time = it2->time.time_since_epoch() - it->time.time_since_epoch();
        rewards_earned += (delta_time.count())*(it->reward_multiplier);
      }
      
      ++it2;      
    }
    if (rewards_earned <= 0 )
    {
      rewards_earned = 1;
    }
    
    rewards_earned = rewards_earned/31536000;
    rewards_earned = rewards_earned * iterator->stakingamount.amount;
    rewards_earned = rewards_earned/100;
    rewards_earned = rewards_earned/1000000;
    asset payable_amount = iterator->stakingamount;
    payable_amount.amount = iterator->stakingamount.amount + rewards_earned;
    add_balance(from, payable_amount, from);
    payable_amount.amount =  rewards_earned;
    only_issue(iterator->key, payable_amount, "");
    payable_amount.amount = (iterator->stakingamount.amount)*(-1);
    add_staking_entries(iterator->key, payable_amount);
    stakerecordstable.erase(iterator);
  }
  else
  {
    //The user is in the table
  }
}

void escrow::add_staking_entries(const name &from, const asset &quantity)
{
  auto sym = quantity.symbol.code();
  stats statstable(get_self(), sym.raw());
  totalstaking latest_staking_entry(get_self(), get_first_receiver().value);
  allstakes all_staking_entries(get_self(), get_first_receiver().value);
  auto existing_currency = statstable.find(sym.raw());
  if (latest_staking_entry.begin() == latest_staking_entry.end())
  {
    latest_staking_entry.emplace(from, [&](auto &row) {
      row.supply = existing_currency->supply;
      row.totalstakedamount = quantity;
      row.time = eosio::current_time_point();
    });
    int64_t reward_multiplier = calculate_reward_multiplier(existing_currency->supply.amount, quantity.amount);
    all_staking_entries.emplace(from, [&](auto &row) {
      row.reward_multiplier = reward_multiplier;
      row.time = eosio::current_time_point();
    });
  }
  else
  {
    auto all_staking_iterator = all_staking_entries.end();
    --all_staking_iterator;
    auto latest_staking_iterator = latest_staking_entry.begin();
    if (all_staking_iterator->time.sec_since_epoch() + STAKING_INTERVAL <= eosio::current_time_point().sec_since_epoch())
    {
      asset totalamount  = latest_staking_iterator->totalstakedamount + quantity;
      int64_t reward_multiplier = calculate_reward_multiplier(existing_currency->supply.amount, totalamount.amount);
      all_staking_entries.emplace(from, [&](auto &row) {
        row.reward_multiplier = reward_multiplier;
        row.time = eosio::current_time_point();
      });
    }

    latest_staking_entry.emplace(from, [&](auto &row) {
      row.supply = existing_currency->supply;
      row.totalstakedamount = latest_staking_iterator->totalstakedamount + quantity;
      row.time = eosio::current_time_point();
    });
    latest_staking_entry.erase(latest_staking_iterator);
  }
}

int64_t escrow::calculate_reward_multiplier(const int64_t& totalsupply, const int64_t& totalstaked) {
  int64_t stakeOfSupplyPercentage = (totalstaked * 100)/totalsupply;
  int64_t reward_multiplier = 1;
  if(stakeOfSupplyPercentage <=1 ) {
    reward_multiplier = 300;
  } else if (stakeOfSupplyPercentage > 1 && stakeOfSupplyPercentage <=3) {
    reward_multiplier = 75/(1 - (3/(4*stakeOfSupplyPercentage)));
  } else if (stakeOfSupplyPercentage > 3 && stakeOfSupplyPercentage <=10) {
    reward_multiplier = 700/(17*(1 - (30/(17*stakeOfSupplyPercentage))));
  } else if (stakeOfSupplyPercentage > 10 && stakeOfSupplyPercentage <=50) {
    reward_multiplier = 1200/(119*(1 - (950/(119*stakeOfSupplyPercentage))));
  } else {
    reward_multiplier = 12/(7*(1 - (300/(7*stakeOfSupplyPercentage))));
  }

  return reward_multiplier;
}

void escrow::retire(const asset &quantity, const string &memo)
{
  auto sym = quantity.symbol;
  check(sym.is_valid(), "invalid symbol name");
  check(memo.size() <= 256, "memo has more than 256 bytes");

  stats statstable(get_self(), sym.code().raw());
  auto existing = statstable.find(sym.code().raw());
  check(existing != statstable.end(), "escrow with symbol does not exist");
  const auto &st = *existing;

  require_auth(st.issuer);
  check(quantity.is_valid(), "invalid quantity");
  check(quantity.amount > 0, "must retire positive quantity");

  check(quantity.symbol == st.supply.symbol, "symbol precision mismatch");

  statstable.modify(st, same_payer, [&](auto &s) {
    s.supply -= quantity;
  });

  sub_balance(st.issuer, quantity);
}

void escrow::transfer(const name &from,
                      const name &to,
                      const asset &quantity,
                      const string &memo)
{
  check(from != to, "cannot transfer to self");
  require_auth(from);
  check(is_account(to), "to account does not exist");
  auto sym = quantity.symbol.code();
  stats statstable(get_self(), sym.raw());
  const auto &st = statstable.get(sym.raw());

  require_recipient(from);
  require_recipient(to);

  check(quantity.is_valid(), "invalid quantity");
  check(quantity.amount > 0, "must transfer positive quantity");
  check(quantity.symbol == st.supply.symbol, "symbol precision mismatch");
  check(memo.size() <= 256, "memo has more than 256 bytes");

  auto payer = has_auth(to) ? to : from;

  sub_balance(from, quantity);
  add_balance(to, quantity, payer);
}

void escrow::sub_balance(const name &owner, const asset &value)
{
  accounts from_acnts(get_self(), owner.value);

  const auto &from = from_acnts.get(value.symbol.code().raw(), "no balance object found");
  check(from.balance.amount >= value.amount, "overdrawn balance");

  from_acnts.modify(from, owner, [&](auto &a) {
    a.balance -= value;
  });
}

void escrow::add_balance(const name &owner, const asset &value, const name &ram_payer)
{
  accounts to_acnts(get_self(), owner.value);
  auto to = to_acnts.find(value.symbol.code().raw());
  if (to == to_acnts.end())
  {
    to_acnts.emplace(ram_payer, [&](auto &a) {
      a.balance = value;
    });
  }
  else
  {
    to_acnts.modify(to, same_payer, [&](auto &a) {
      a.balance += value;
    });
  }
}

void escrow::open(const name &owner, const symbol &symbol, const name &ram_payer)
{
  require_auth(ram_payer);

  check(is_account(owner), "owner account does not exist");

  auto sym_code_raw = symbol.code().raw();
  stats statstable(get_self(), sym_code_raw);
  const auto &st = statstable.get(sym_code_raw, "symbol does not exist");
  check(st.supply.symbol == symbol, "symbol precision mismatch");

  accounts acnts(get_self(), owner.value);
  auto it = acnts.find(sym_code_raw);
  if (it == acnts.end())
  {
    acnts.emplace(ram_payer, [&](auto &a) {
      a.balance = asset{0, symbol};
    });
  }
}

void escrow::close(const name &owner, const symbol &symbol)
{
  require_auth(owner);
  accounts acnts(get_self(), owner.value);
  auto it = acnts.find(symbol.code().raw());
  check(it != acnts.end(), "Balance row already deleted or never existed. Action won't have any effect.");
  check(it->balance.amount == 0, "Cannot close because the balance is not zero.");
  acnts.erase(it);
}

} // namespace eosio
