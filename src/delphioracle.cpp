/*

  delphioracle

  Authors: Guillaume "Gnome" Babin-Tremblay - EOS Titan, Andrew "netuoso" Chaney - EOS Titan

  Website: https://eostitan.com
  Email: guillaume@eostitan.com

  Github: https://github.com/eostitan/delphioracle/

  Published under MIT License

*/

#include <delphioracle.hpp>
#include <custom_ctime.hpp>
#include <algorithm>
#include <numeric>
#include <iterator>

namespace {
  const std::map<median_types, uint8_t> limits = {
    {median_types::day,          1},
    {median_types::current_week, 1},
    {median_types::week,         4},
    {median_types::month,        12}
  };

    const std::map<median_types, uint32_t> time_consts = {
    {median_types::day,          86400},
    {median_types::current_week, 86400 * 7},
    {median_types::week,         86400 * 7},
    {median_types::month,        86400 * 7 * 4}
  };

  const std::map<average_types, uint8_t> average_number_of_days = {
      {average_types::last_7_days,  7},
      {average_types::last_14_days, 14},
      {average_types::last_30_days, 30},
      {average_types::last_45_days, 45}
    };
}

//Write datapoint
ACTION delphioracle::write(const name owner, const std::vector<quote>& quotes) {
  require_auth(owner);

  const int length = quotes.size();
  //print("quotes length ", length, "\n");

  check(length > 0, "must supply non-empty array of quotes");
  check(check_oracle(owner), "account is not a qualified oracle");
  //print("Oracle passed check_oracle");

  statstable stable(_self, _self.value);
  pairstable pairs(_self, _self.value);

  auto oitr = stable.find(owner.value);
  //print("Found the stable for owner.value");

  for (int i = 0; i < length; i++) {
    //print("quote ", i, " ", quotes[i].value, " ",  quotes[i].pair, "\n");

    auto itr = pairs.find(quotes[i].pair.value);

    check(itr != pairs.end() && itr->active == true, "pair not allowed");

    check_last_push(owner, quotes[i].pair);

    if (itr->bounty_amount >= one_larimer && oitr != stable.end()) {

      //global donation to the contract, split between top oracles across all pairs
      stable.modify(*oitr, _self, [&]( auto& s ) {
        s.balance += one_larimer;
      });

      //global donation to the contract, split between top oracles across all pairs
      pairs.modify(*itr, _self, [&]( auto& s ) {
        s.bounty_amount -= one_larimer;
      });
    }
    else if (itr->bounty_awarded == false && itr->bounty_amount < one_larimer)  {

      //global donation to the contract, split between top oracles across all pairs
      pairs.modify(*itr, _self, [&]( auto& s ) {
        s.bounty_awarded = true;
      });
    }

    update_datapoints(owner, quotes[i].value, itr);
    update_medians(owner, quotes[i].value, itr);

    // The last day averages code does not need to be updated on every write - prevent from executing it too fast
    globaltable gtable(_self, _self.value);
    auto gitr = gtable.begin();

    int32_t next_run_sec = gitr->last_daily_average_run + gitr->daily_average_timeout;
    int32_t current_time_sec = current_time_point().sec_since_epoch();

    if (current_time_sec > next_run_sec) {
        gtable.modify(gitr, _self, [&](auto& global_value) {
            global_value.last_daily_average_run = current_time_sec;
        });

        update_daily_datapoints(itr->name);
        update_averages(itr->name);
    }
  }
}

//claim rewards
ACTION delphioracle::claim(name owner) {
  require_auth(owner);

  globaltable gtable(_self, _self.value);
  statstable sstore(_self, _self.value);

  auto itr = sstore.find(owner.value);
  auto gitr = gtable.begin();

  check(itr != sstore.end(), "oracle not found");
  check( itr->balance.amount > 0, "no rewards to claim" );

  asset payout = itr->balance;

  sstore.modify( *itr, _self, [&]( auto& a ) {
      a.balance = asset(0, symbol("TLOS", 4));
      a.last_claim = current_time_point();
  });

  gtable.modify( *gitr, _self, [&]( auto& a ) {
      a.total_claimed += payout;
  });

  action act(
    permission_level{_self, "active"_n},
    "eosio.token"_n, "transfer"_n,
    std::make_tuple(_self, owner, payout, std::string(""))
  );
  act.send();
}

//temp configuration
ACTION delphioracle::configure(globalinput g) {
  require_auth(_self);

  globaltable gtable(_self, _self.value);
  pairstable pairs(_self, _self.value);

  auto gitr = gtable.begin();
  auto pitr = pairs.begin();

  if (gitr == gtable.end()) {
    // First time running the configuration, retrieving from the old table
    old_globaltable old_gtable(_self, _self.value);
    int total_datapoints_count = 0;
    asset total_claimed = asset(0, symbol("TLOS", 4));

    auto old_gitr = old_gtable.begin();
    if (old_gitr != old_gtable.end()) {
        total_datapoints_count = old_gitr->total_datapoints_count;
        total_claimed = old_gitr->total_claimed;
    }

    gtable.emplace(_self, [&](auto& o) {
      o.id = 1;
      o.total_datapoints_count = total_datapoints_count;
      o.total_claimed = total_claimed;
      o.datapoints_per_instrument = g.datapoints_per_instrument;
      o.bars_per_instrument = g.bars_per_instrument;
      o.vote_interval = g.vote_interval;
      o.write_cooldown = g.write_cooldown;
      o.approver_threshold = g.approver_threshold;
      o.approving_oracles_threshold = g.approving_oracles_threshold;
      o.approving_custodians_threshold = g.approving_custodians_threshold;
      o.minimum_rank = g.minimum_rank;
      o.paid = g.paid;
      o.min_bounty_delay = g.min_bounty_delay;
      o.new_bounty_delay = g.new_bounty_delay;
      o.last_daily_average_run = 0;
    });
  } else {
    gtable.modify(*gitr, _self, [&]( auto& o ) {
      o.datapoints_per_instrument = g.datapoints_per_instrument;
      o.bars_per_instrument = g.bars_per_instrument;
      o.vote_interval = g.vote_interval;
      o.write_cooldown = g.write_cooldown;
      o.approver_threshold = g.approver_threshold;
      o.approving_oracles_threshold = g.approving_oracles_threshold;
      o.approving_custodians_threshold = g.approving_custodians_threshold;
      o.minimum_rank = g.minimum_rank;
      o.paid = g.paid;
      o.min_bounty_delay = g.min_bounty_delay;
      o.new_bounty_delay = g.new_bounty_delay;
      o.last_daily_average_run = 0;
    });
  }

  if (pitr == pairs.end()) {
      pairs.emplace(_self, [&](auto& o) {
        o.active = true;
        o.bounty_awarded = true;
        o.bounty_edited_by_custodians = true;
        o.proposer = _self;
        o.name = "tlosusd"_n;
        o.bounty_amount = asset(0, symbol("TLOS", 4));
        o.base_symbol =  symbol("TLOS", 4);
        o.base_type = e_asset_type::eosio_token;
        o.base_contract = "eosio.token"_n;
        o.quote_symbol = symbol("USD", 2);
        o.quote_type = e_asset_type::fiat;
        o.quote_contract = ""_n;
        o.quoted_precision = 4;
      });

      datapointstable dstore(_self, name("tlosusd"_n).value);

      //First data point starts at uint64 max
      uint64_t primary_key = 0;
      for (uint16_t i = 0; i < 21; i++) {
        //Insert next datapoint
        auto c_itr = dstore.emplace(_self, [&](auto& s) {
          s.id = primary_key;
          s.value = 0;
          s.timestamp = NULL_TIME_POINT;
        });

        primary_key++;
      }

      {
        make_records_for_medians_table(median_types::day,          "tlosusd"_n, get_self(), medians());
        make_records_for_medians_table(median_types::current_week, "tlosusd"_n, get_self(), medians());
        make_records_for_medians_table(median_types::week,         "tlosusd"_n, get_self(), medians());
        make_records_for_medians_table(median_types::month,        "tlosusd"_n, get_self(), medians());
      }
  }
}

//Delphi Oracle - Bounty logic

//Anyone can propose a bounty to add a new pair. This is the only way to add new pairs.
//  By proposing a bounty, the proposer pays upfront for all the RAM requirements of the pair (expensive enough to discourage spammy proposals)

//Once the bounty has been created, anyone can contribute to the bounty by sending a transfer with the bounty name in the memo

//Custodians of the contract or the bounty proposer can cancel the bounty. This refunds RAM to the proposer, as well as all donations made to the bounty
//  to original payer accounts.

//Custodians of the contract can edit the bounty's name and description (curation and standardization process)

//Any BP that has contributed a certain amount of datapoints (TBD) to the contract is automatically added as an authorized account to approve a bounty

//Once a BP approves the bounty, a timer (1 week?) starts

//X more BPs and Y custodians (1?) must then approve the bounty to activate it

//The pair is not activated until the timer expires AND X BPs and Y custodians approved

//No more than 1 pair can be activated per X period of time (72 hours?)

//The bounty is then paid at a rate of X larimers per datapoint to BPs contributing to it until it runs out.


//create a new pair request bounty
ACTION delphioracle::newbounty(name proposer, pairinput pair) {
  require_auth(proposer);

  //Add request, proposer pays the RAM for the request + data structure for datapoints & bars.

  pairstable pairs(_self, _self.value);
  datapointstable dstore(_self, pair.name.value);

  auto itr = pairs.find(pair.name.value);

  check(pair.name != "system"_n, "Cannot create a pair named system");
  check(itr == pairs.end(), "A pair with this name already exists.");

  pairs.emplace(proposer, [&](auto& s) {
    s.proposer = proposer;
    s.name = pair.name;
    s.base_symbol = pair.base_symbol;
    s.base_type = pair.base_type;
    s.base_contract = pair.base_contract;
    s.quote_symbol = pair.quote_symbol;
    s.quote_type = pair.quote_type;
    s.quote_contract = pair.quote_contract;
    s.quoted_precision = pair.quoted_precision;
  });

  //First data point starts at uint64 max
  uint64_t primary_key = 0;
  for (uint16_t i = 0; i < 21; i++) {
    //Insert next datapoint
    auto c_itr = dstore.emplace(proposer, [&](auto& s) {
      s.id = primary_key;
      s.value = 0;
      s.timestamp = NULL_TIME_POINT;
    });

    primary_key++;
  }

  { // for get medians
    make_records_for_medians_table(median_types::day,          pair.name, proposer, medians());
    make_records_for_medians_table(median_types::current_week, pair.name, proposer, medians());
    make_records_for_medians_table(median_types::week,         pair.name, proposer, medians());
    make_records_for_medians_table(median_types::month,        pair.name, proposer, medians());
  }
}

//cancel a bounty
ACTION delphioracle::cancelbounty(name name, std::string reason) {
  pairstable pairs(_self, _self.value);
  datapointstable dstore(_self, name.value);

  auto itr = pairs.find(name.value);
  check(itr != pairs.end(), "bounty doesn't exist");

  //print("itr->proposer", itr->proposer, "\n");

  check(has_auth(_self) || has_auth(itr->proposer), "missing required authority of contract or proposer");
  check(itr->active == false, "cannot cancel live pair");

  //Cancel bounty, post reason to chain.

  pairs.erase(itr);

  while (dstore.begin() != dstore.end()) {
    auto ditr = dstore.end();
    ditr--;
    dstore.erase(ditr);
  }
  //TODO: Refund accumulated bounty to balance of user

  erase_medians(name);
}

//vote bounty
ACTION delphioracle::votebounty(name owner, name bounty) {
  require_auth(owner);

  pairstable pairs(_self, _self.value);
  auto pitr = pairs.find(bounty.value);

  check(!pitr->active, "pair is already active.");
  check(pitr != pairs.end(), "bounty not found.");

  custodianstable custodians(_self, _self.value);
  auto itr = custodians.find(owner.value);

  bool vote_approved = false;
  std::string err_msg = "";

  //print("itr->name", itr->name, "\n");

  if (itr != custodians.end()) {
    //voter is custodian
    //print("custodian found \n");

    std::vector<eosio::name> cv = pitr->approving_custodians;
    auto citr = find(cv.begin(), cv.end(), owner);

    //check(citr == cv.end(), "custodian already voting for bounty");

    if (citr == cv.end()) {
      cv.push_back(owner);
      pairs.modify(*pitr, _self, [&]( auto& s ) {
        s.approving_custodians = cv;
      });

      //print("custodian added vote \n");

      vote_approved = true;
    }
    else 
      err_msg = "custodian already voting for bounty";
  }

  //print("checking oracle qualification... \n");

  if (check_approver(owner)) {
    std::vector<eosio::name> ov = pitr->approving_oracles;
    auto oitr = find(ov.begin(), ov.end(), owner);
    if (oitr == ov.end()) {
      ov.push_back(owner);
      pairs.modify(*pitr, _self, [&]( auto& s ) {
        s.approving_oracles = ov;
      });

      //print("oracle added vote \n");

      vote_approved = true;
    }
    else 
      err_msg = "oracle already voting for bounty";

  }
  else err_msg = "owner not a qualified oracle";

  check(vote_approved, err_msg.c_str());

  globaltable gtable(_self, _self.value);
  auto gitr = gtable.begin();

  uint64_t approving_custodians_count = std::distance(pitr->approving_custodians.begin(), pitr->approving_custodians.end());
  uint64_t approving_oracles_count = std::distance(pitr->approving_oracles.begin(), pitr->approving_oracles.end());

  if (approving_custodians_count >= gitr->approving_custodians_threshold 
    && approving_oracles_count >= gitr->approving_oracles_threshold) {
      //print("activate bounty", "\n");
      
      pairs.modify(*pitr, _self, [&]( auto& s ) {
        s.active = true;
      });
  }
}

//vote bounty
ACTION delphioracle::unvotebounty(name owner, name bounty) {
  require_auth(owner);

  pairstable pairs(_self, _self.value);
  auto pitr = pairs.find(bounty.value);

  check(!pitr->active, "pair is already active.");
  check(pitr != pairs.end(), "bounty not found.");

  custodianstable custodians(_self, _self.value);
  auto itr = custodians.find(owner.value);
  //print("itr->name", itr->name, "\n");

  if (itr != custodians.end()) {
    //voter is custodian
    //print("custodian found \n");

    std::vector<eosio::name> cv = pitr->approving_custodians;
    auto citr = find(cv.begin(), cv.end(), owner);
    check(citr != cv.end(), "custodian is not voting for bounty");
    cv.erase(citr);

    pairs.modify(*pitr, _self, [&]( auto& s ) {
      s.approving_oracles = cv;
    });

    //print("custodian removed vote \n");
  } else {
    //print("checking oracle qualification... \n");

    //check(check_approver(owner), "owner not a qualified oracle"); // not necessary

    std::vector<eosio::name> ov = pitr->approving_oracles;
    auto oitr = find(ov.begin(), ov.end(), owner);
    check(oitr != ov.end(), "not an oracle or oracle is not voting for bounty");
    ov.erase(oitr);

    pairs.modify(*pitr, _self, [&]( auto& s ) {
      s.approving_oracles = ov;
    });

    //print("oracle removed vote \n");
  }
}

//add custodian
ACTION delphioracle::addcustodian(name name) {
  require_auth(_self);

  custodianstable custodians(_self, _self.value);
  custodians.emplace(_self, [&](auto& s) {
    s.name = name;
  });
}

//remove custodian
ACTION delphioracle::delcustodian(name name) {
  require_auth(_self);

  custodianstable custodians(_self, _self.value);
  auto itr = custodians.find(name.value);
  check(itr != custodians.end(), "account not a custodian");
  custodians.erase(itr);
}

//registers a user
ACTION delphioracle::reguser(name owner) {
  require_auth(owner);
  if( !check_user(owner) ) 
    create_user( owner );
}

//updates all users voting scores
//run at some random interval daily
ACTION delphioracle::updateusers() {
  require_auth( _self );

  userstable users(_self, _self.value);
  voters_table vtable("eosio"_n, name("eosio").value);

  for(auto itr = users.begin(); itr != users.end(); ++itr) {
    // add proxy score
    auto v_itr = vtable.find(itr->name.value);
    auto score = itr->score;

    if( v_itr != vtable.end() && v_itr->proxy == _self) {
      score += v_itr->staked;
    }

    users.modify(*itr, _self, [&]( auto& o ) {
      o.score = score;
    });
  }
}

//Clear all data
ACTION delphioracle::clear(name pair) {
  require_auth(_self);

  globaltable gtable(_self, _self.value);
  statstable gstore(_self, _self.value);
  statstable lstore(_self, pair.value);
  datapointstable estore(_self,  pair.value);
  pairstable pairs(_self, _self.value);
  custodianstable ctable(_self, _self.value);

  while (ctable.begin() != ctable.end()) {
      auto itr = ctable.end();
      itr--;
      ctable.erase(itr);
  }

  while (gtable.begin() != gtable.end()) {
      auto itr = gtable.end();
      itr--;
      gtable.erase(itr);
  }

  while (gstore.begin() != gstore.end()) {
      auto itr = gstore.end();
      itr--;
      gstore.erase(itr);
  }

  while (lstore.begin() != lstore.end()) {
      auto itr = lstore.end();
      itr--;
      lstore.erase(itr);
  }

  while (estore.begin() != estore.end()) {
      auto itr = estore.end();
      itr--;
      estore.erase(itr);
  }

  while (pairs.begin() != pairs.end()) {
      auto itr = pairs.end();
      itr--;
      pairs.erase(itr);
  }
}

ACTION delphioracle::voteabuser(const name owner, const name abuser) {
  require_auth(owner);
  check(check_oracle(abuser), "abuser is not a qualified oracle");

  donationstable donations(_self, owner.value);
  voters_table vtable("eosio"_n, name("eosio").value);

  // donations
  auto d_idx = donations.get_index<"donator"_n>();
  auto d_itr = d_idx.find(owner.value);

  auto total_donated = 0;
  while (d_itr->donator == owner && d_itr != d_idx.end()) {
    total_donated += d_itr->amount.amount;
    d_itr++;
  }

  auto v_itr = vtable.find(owner.value);

  // proxy voting
  auto total_proxied = 0;
  if( v_itr != vtable.end() && v_itr->proxy == _self) {
    total_proxied += v_itr->staked;
  }

  // TODO: verify user object exists and user has some voting score
  check(total_donated > 0 || total_proxied > 0, "user must donate or proxy vote to delphioracle to vote for abusers");
  //print("user: ", owner, " is voting for abuser: ", abuser, " with total stake: ", total_donated + total_proxied);
  // store data for abuse vote
}

ACTION delphioracle::makemedians() {
  require_auth(get_self());

  if (!is_medians_active()) {
    return;
  }
  
  pairstable pairs(_self, _self.value);
  for (auto itr = pairs.begin(); itr != pairs.end(); ++itr) {
    make_records_for_medians_table(median_types::day,          itr->name, get_self(), medians());
    make_records_for_medians_table(median_types::current_week, itr->name, get_self(), medians());
    make_records_for_medians_table(median_types::week,         itr->name, get_self(), medians());
    make_records_for_medians_table(median_types::month,        itr->name, get_self(), medians());
  }
}

void delphioracle::make_records_for_medians_table(median_types type, const name& pair, const name& payer, const medians& default_median) {
    if (!is_medians_active()) {
    return;
  }
  
  medianstable medians_table(get_self(), pair.value);
  const auto count_type_elements = std::count_if(medians_table.begin(), medians_table.end(), 
    [&](medians medians_obj) { 
      return medians_obj.type == medians::get_type(type); 
  });

  if (count_type_elements != limits.at(type)) {
    const auto need_make_elements = limits.at(type) - count_type_elements;
    for (auto counter = 0; counter < need_make_elements; ++counter) {
      medians_table.emplace(payer, [&](auto& medians_obj) {
        medians_obj.id = medians_table.available_primary_key();
        medians_obj.type = medians::get_type(type);
        medians_obj.value = default_median.value;
        medians_obj.request_count = default_median.request_count;
        medians_obj.timestamp = default_median.timestamp;
      });
    }
  }
}

const time_point delphioracle::get_round_up_current_time(median_types type) const {
  time_t current_time_sec = static_cast<time_t>(current_time_point().sec_since_epoch());

  if (!_is_active_current_week_cashe) {
    current_time_sec += time_consts.at(median_types::day) * 20;
  }

  auto get_type_time = [&]() -> time_point {
  auto itr = time_consts.find(type);
    if (itr != time_consts.end()) {
      auto remainder = current_time_sec % itr->second;
      return time_point_sec(current_time_sec - remainder);
    }

    return NULL_TIME_POINT;
  };

  auto get_type_month = [&]() -> time_point {
    auto struct_current_time = custom_ctime::gmtime(&current_time_sec);
    
    check(struct_current_time != nullptr, "error get current month");
    
    struct_current_time->tm_sec = 0; 
    struct_current_time->tm_min = 0;
    struct_current_time->tm_hour = 0;
    struct_current_time->tm_mday = 1; 

    auto current_time = custom_ctime::mktime(struct_current_time);
    return time_point_sec(static_cast<int32_t>(current_time));
  };

  switch (type)
  {
  case median_types::day:
    return get_type_time();
  case median_types::current_week:
    return get_type_time();
  case median_types::week:
    return get_type_time();
  case median_types::month:
    return _is_active_current_week_cashe ? get_type_month() : get_type_time();
  default: {}
  }

  return NULL_TIME_POINT;
}

bool delphioracle::is_in_time_range(median_types type, const time_point& start_time_range, 
  const time_point& time_value, bool is_previous_value) const {
  time_point select_time_value = time_value;

  auto is_in_time_consts_range = [&]() {
    auto itr = time_consts.find(type);
    if (itr != time_consts.end()) {
      if (is_previous_value) {
        select_time_value -= seconds(itr->second);
      }
      time_point end_time_range = start_time_range + time_point(seconds(itr->second));
      return (start_time_range <= select_time_value) && (select_time_value < end_time_range);
    }
    return false;
  };

  auto is_in_time_month_range = [&]() {
    std::tm* result = nullptr;
    
    time_t current_time_sec = static_cast<time_t>(select_time_value.sec_since_epoch());
    result = custom_ctime::gmtime(&current_time_sec);
    check(result != nullptr, "error get current time in range");
    auto struct_current_time = *result;

    time_t start_time_range_sec = static_cast<time_t>(start_time_range.sec_since_epoch());
    result = custom_ctime::gmtime(&start_time_range_sec);
    check(result != nullptr, "error get start time in range");
    auto struct_start_time_range = *result;

    return struct_start_time_range.tm_year == struct_current_time.tm_year 
        && struct_start_time_range.tm_mon  == struct_current_time.tm_mon 
        && struct_start_time_range.tm_mday <= struct_current_time.tm_mday;
  };

  switch (type)
  {
  case median_types::day:
    return is_in_time_consts_range();
  case median_types::current_week:
    return is_in_time_consts_range();
  case median_types::week:
    return is_in_time_consts_range();
  case median_types::month:
    return _is_active_current_week_cashe ? is_in_time_month_range() : is_in_time_consts_range();
  default: {}
  }

  return false;
}

void delphioracle::erase_medians(const name& pair) {
  medianstable medians(get_self(), pair.value);
  
  while (medians.begin() != medians.end()) {
      auto itr = medians.end();
      itr--;
      medians.erase(itr);
  }
}

void delphioracle::update_medians(const name& owner, const uint64_t value, pairstable::const_iterator pair_itr) {  
  if (!is_medians_active()) {
    return;
  }

  _is_active_current_week_cashe = is_active_current_week();
  
  medianstable medians_table(get_self(), pair_itr->name.value);
  const auto count_elements = std::distance(medians_table.begin(), medians_table.end());

  if (count_elements > 0) {
    update_medians_by_types(median_types::day, owner, pair_itr->name, get_round_up_current_time(median_types::day), value);
  }
}

bool delphioracle::is_active_current_week() const {
  medianstable medians_table(get_self(), name("tlosusd").value);
  for (auto itr = medians_table.begin(); itr != medians_table.end(); ++itr) {
    if (itr->type == medians::get_type(median_types::current_week)) {
      return true;
    }
  }

  return false;
}

void delphioracle::update_medians_by_types(median_types type, const name& owner, const name& pair,
  const time_point& median_timestamp, const uint64_t median_value, const uint64_t median_request_count) {

  medianstable medians_table(get_self(), pair.value);
  auto medians_timestamp_index = medians_table.get_index<"timestamp"_n>();

  struct short_type_medians {
    uint64_t id = 0;
    time_point timestamp = NULL_TIME_POINT;
  };

  std::vector<short_type_medians> short_medians_elements;
  for (auto itr = medians_timestamp_index.begin(); itr != medians_timestamp_index.end(); ++itr) {
    if (itr->type == medians::get_type(type)) {
      short_type_medians obj{itr->id, itr->timestamp};
      short_medians_elements.push_back(std::move(obj));
    }
  }

  auto short_medians_index = std::find_if(short_medians_elements.begin(), short_medians_elements.end(),
      [&](const short_type_medians &short_medians_element) {
        return is_in_time_range(type, short_medians_element.timestamp, median_timestamp);
  });

  switch (type)
  {
  case median_types::week:
  {
    if (_is_active_current_week_cashe) {
      auto update_itr = medians_table.find(short_medians_elements.begin()->id);
      medians_table.modify(update_itr, owner, [&](medians &obj) {
        obj.value = median_value;
        obj.request_count = median_request_count;
        obj.timestamp = median_timestamp;
      });

      break;
    }
  }
  case median_types::day: // update for current_week, month - new implementation
  case median_types::current_week: // update for week - new implementation
  case median_types::month:
  {
    if (short_medians_index != short_medians_elements.end()) {
      auto medians_table_index = medians_table.find(short_medians_index->id);
      medians_table.modify(medians_table_index, owner, [&](medians &obj) {        
        obj.value += median_value;
        obj.request_count += median_request_count;
      });
    } else {
      auto update_itr = medians_table.find(short_medians_elements.begin()->id);
      auto temp_medians_value = update_itr->value;
      auto temp_medians_timestamp = update_itr->timestamp;
      auto temp_medians_request_count = update_itr->request_count;

      // TODO ingore this check, so we have only one record for day or current week
      if (type != median_types::day || type != median_types::current_week) { 
        auto prev_short_medians_index = std::find_if(short_medians_elements.begin(), short_medians_elements.end(), 
        [&](const short_type_medians &short_medians_element) { 
          return is_in_time_range(type, short_medians_element.timestamp, median_timestamp, true); 
        });

        if (prev_short_medians_index != short_medians_elements.end())
        {
          auto prev_medians_itr = medians_table.find(prev_short_medians_index->id);
          temp_medians_value = prev_medians_itr->value;
          temp_medians_timestamp = prev_medians_itr->timestamp;  
          temp_medians_request_count = prev_medians_itr->request_count; 
        }
      }

      medians_table.modify(*update_itr, owner, [&](medians &obj) {
        obj.value = median_value;
        obj.request_count = median_request_count;
        obj.timestamp = get_round_up_current_time(type);
      });

      if (temp_medians_value != 0 && temp_medians_request_count != 0) {
        for (auto type : GetUpdateMedians(type)) {
          update_medians_by_types(type, owner, pair, temp_medians_timestamp, temp_medians_value, temp_medians_request_count);
        }
      }
    }
  }
  default: {}
  }
}

std::vector<median_types> delphioracle::GetUpdateMedians(median_types current_type) const {
  using vect = std::vector<median_types>;
  switch (current_type)
  {
    case median_types::day: // update for current_week, month - new implementation
      return _is_active_current_week_cashe ? 
        vect{median_types::current_week, median_types::month} : vect{median_types::week};
    case median_types::current_week:
      return vect{median_types::week};
    case median_types::week:
      return _is_active_current_week_cashe ? vect() : vect{median_types::month};
    default: return vect();
  }
}

ACTION delphioracle::initmedians(bool is_active) {
  require_auth(get_self());
  
  flagmedians flag_medians_obj; 
  singleton_flag_medians flag_medians_instance(get_self(), get_self().value);

  auto obj = flag_medians_instance.get_or_create(get_self(), flag_medians_obj);
  obj.is_active = is_active;
  flag_medians_instance.set(obj, get_self());
}

ACTION delphioracle::updtversion() {
  require_auth(get_self());

  check(is_medians_active(), "not active medians");
  check(!is_active_current_week(), "curent week record exist, you using actual contract version");
 
  pairstable pairs(_self, _self.value);
  for (auto pair_itr = pairs.begin(); pair_itr != pairs.end(); ++pair_itr) {  

    medianstable medians_table(get_self(), pair_itr->name.value);
    auto medians_timestamp_index = medians_table.get_index<"timestamp"_n>();    

    medians temp_current_week;
    for (auto itr = medians_timestamp_index.begin(); itr != medians_timestamp_index.end(); ++itr) {
      if (itr->type == medians::get_type(median_types::week) 
        && is_in_time_range(median_types::week, itr->timestamp, get_round_up_current_time(median_types::day))) {
          temp_current_week = *itr;
          medians_timestamp_index.modify(itr, get_self(), [&](medians &obj) {
            obj.value = 0;
            obj.request_count = 0;
            obj.timestamp = NULL_TIME_POINT;  
          });
          break;
      }
    }

    for (auto itr = medians_timestamp_index.begin(); itr != medians_timestamp_index.end(); ++itr) {
      if (itr->type == medians::get_type(median_types::month) 
        && is_in_time_range(median_types::month, itr->timestamp, get_round_up_current_time(median_types::day))) {          
          medians_timestamp_index.modify(itr, get_self(), [&](medians &obj) {
            obj.value += temp_current_week.value;
            obj.request_count += temp_current_week.request_count;
          });

          break;
      }
    }

    make_records_for_medians_table(median_types::current_week, pair_itr->name, get_self(), temp_current_week);

    for (auto itr_medians = medians_table.begin(); itr_medians != medians_table.end(); ++itr_medians) {
      if (itr_medians->timestamp != NULL_TIME_POINT) {
        medians_table.modify(*itr_medians, get_self(), [&](medians &obj) {
          uint32_t timestamp_sec = obj.timestamp.sec_since_epoch();
          obj.timestamp = time_point(seconds(timestamp_sec - time_consts.at(median_types::day) * 20)); // erase bias in prod
        });
      }
    }
  }
}

/**
    Updates the daily datapoints with the daily median
    - Gets the daily median
    - If there are `daily_datapoints_per_instrument` (or more) it will replace the first one
      updating the timestamp.
    - If there are less than `daily_datapoints_per_instrument` it will append it
*/
void delphioracle::update_daily_datapoints(name instrument) {
    std::optional<std::pair<time_point, uint64_t>> daily_median = get_daily_median(instrument);
    if (!daily_median) {
        return;
    }

    dailydatapointstable daily_datapoints_table(get_self(), instrument.value);
    auto daily_datapoints_timestamp_index = daily_datapoints_table.get_index<"timestamp"_n>();
    size_t count = std::distance(daily_datapoints_timestamp_index.begin(), daily_datapoints_timestamp_index.end());

    globaltable gtable(_self, _self.value);
    auto gitr = gtable.begin();

    auto last_datapoint = daily_datapoints_timestamp_index.rbegin();

    if (last_datapoint != daily_datapoints_timestamp_index.rend() && last_datapoint->timestamp == daily_median->first) {
        // We are on the same day, just update
        daily_datapoints_table.modify(
            *last_datapoint,
            _self,
            [&](auto& datapoint) {
                datapoint.value = daily_median->second;
            }
        );
    } else if (count > gitr->daily_datapoints_per_instrument) {
        auto target_record = daily_datapoints_timestamp_index.rbegin();
        daily_datapoints_timestamp_index.modify(
            daily_datapoints_timestamp_index.begin(),
            _self,
            [&](auto& datapoint) {
                datapoint.value = daily_median->second;
                datapoint.timestamp = daily_median->first;
            }
        );
    } else {
        daily_datapoints_table.emplace(_self, [&](auto& datapoint) {
            datapoint.id = daily_datapoints_table.available_primary_key();
            datapoint.value = daily_median->second;
            datapoint.timestamp = daily_median->first;
        });
    }
}

/**
    Computes the last day averages using the daily datapoints
    Fetches the last day averages
*/
uint64_t delphioracle::compute_last_days_average(name instrument, uint8_t days) {
    dailydatapointstable daily_datapoints_table(get_self(), instrument.value);

    auto daily_datapoints_timestamp_index = daily_datapoints_table.get_index<"timestamp"_n>();
    days = std::min(
        days,
        static_cast<uint8_t>(std::distance(daily_datapoints_timestamp_index.begin(), daily_datapoints_timestamp_index.end()))
    );

    if (days == 0) {
        return 0;
    }

    auto past_days = daily_datapoints_timestamp_index.rbegin();
    std::advance(past_days, days);

    return std::accumulate(
        daily_datapoints_timestamp_index.rbegin(),
        past_days,
        0,
        [](auto& prev, auto& data_point) {
            return prev + data_point.value;
        }
    ) / days;
}

void delphioracle::update_averages(name instrument) {
    averagestable averages_table(_self, instrument.value);

    // iterate average_number_of_days
    for (auto const& it : average_number_of_days) {
        average_types type = it.first;
        uint8_t days = it.second;

        uint64_t average = compute_last_days_average(instrument, days);

        auto average_entry = std::find_if(averages_table.begin(), averages_table.end(),
            [&](auto& entry) {
                return entry.type == averages::get_type(type);
            }
        );

        if (average_entry == averages_table.end()) {
            averages_table.emplace(_self, [&](auto& entry) {
                entry.id = averages_table.available_primary_key();
                entry.type = averages::get_type(type);
                entry.value = average;
                entry.timestamp = current_time_point();
            });
        } else {
            averages_table.modify(average_entry, _self, [&](auto& entry) {
                entry.value = average;
                entry.timestamp = current_time_point();
            });
        }
    }
}

std::optional<std::pair<time_point, uint64_t>> delphioracle::get_daily_median(name instrument) {
    medianstable medians_table(_self, instrument.value);
    auto medians_timestamp_index = medians_table.get_index<"timestamp"_n>();

    auto daily_median = std::find_if(medians_timestamp_index.rbegin(), medians_timestamp_index.rend(),
        [&](auto& median) {
            return median.type == medians::get_type(median_types::day);
    });

    if (daily_median != medians_timestamp_index.rend()) {
        return std::pair(
            daily_median->timestamp,
            daily_median->value / daily_median->request_count
        );
    }

    return {};
}
