// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#include "kraft/raft_driver.h"

#include <utility>

namespace kraft {

RaftDriver::RaftDriver(Raft &raft, Options options)
  : raft_(raft)
  , options_(options)
  , rng_(options.random_seed)
{
  // A zero timeout would make the driver fire on every tick, i.e. spin.
  if (options_.election_timeout_ms == 0) {
    options_.election_timeout_ms = 1;
  }
  if (options_.heartbeat_interval_ms == 0) {
    options_.heartbeat_interval_ms = 1;
  }

  // Chain rather than overwrite: the host may have installed its own
  // notification callbacks before handing the instance to a driver, and both
  // must keep firing.
  prev_on_role_change_ = std::move(raft_.on_role_change_cb_);
  prev_on_commit_ = std::move(raft_.on_commit_index_cb_);

  raft_.on_role_change_cb_ = [this](RaftRole role) {
    if (prev_on_role_change_) {
      prev_on_role_change_(role);
    }
    if (on_role_change_) {
      on_role_change_(role);
    }
  };

  raft_.on_commit_index_cb_ = [this](u64 index) {
    if (prev_on_commit_) {
      prev_on_commit_(index);
    }
    if (on_commit_) {
      on_commit_(index);
    }
  };
}

RaftDriver::~RaftDriver() noexcept
{
  // swap(), not assignment: restoring the instance's callbacks must not
  // allocate inside a destructor. Swapping with an empty function correctly
  // restores "the host had none".
  raft_.on_role_change_cb_.swap(prev_on_role_change_);
  raft_.on_commit_index_cb_.swap(prev_on_commit_);
}

auto RaftDriver::Tick() -> u64
{
  // Nothing the instance can do before the host installed its storage/IO
  // callbacks, and ElectionPeriodic() would return without moving any of the
  // state the deadlines are derived from, so report a whole timeout instead
  // of spinning at 0.
  if (!raft_.IsReady()) {
    return options_.election_timeout_ms;
  }

  const u64 now = raft_.NowMs();
  UpdateDeadline(now);

  // Commit advance and application, for a leader and a follower alike.
  raft_.Periodic();

  if (raft_.IsLeader()) {
    if (now >= next_heartbeat_ms_) {
      raft_.HeartBeatPeriodic();
      next_heartbeat_ms_ = now + options_.heartbeat_interval_ms;
    }
  } else if (election_deadline_ms_ && now >= *election_deadline_ms_) {
    raft_.ElectionPeriodic();
    // Consume the deadline before the trailing UpdateDeadline() runs, which
    // then arms a fresh random one. The state/term change from a real election
    // would re-arm it anyway; clearing it here also covers an ElectionPeriodic()
    // that changed nothing. An instance that just won the election is a leader
    // by then and switches to the heartbeat cadence instead.
    // 避免ElectionPeriodic()没有改变状态，
    // 导致下一次Tick()又触发ElectionPeriodic()，
    // 所以这里清空election_deadline_ms_，让UpdateDeadline()重新设置一个新的election_deadline_ms_
    election_deadline_ms_.reset();
  }

  UpdateDeadline(raft_.NowMs());

  return NextTimeoutMs();
}

auto RaftDriver::NextTimeoutMs() const noexcept -> u64
{
  const u64 now = raft_.NowMs();

  if (raft_.IsLeader()) {
    return next_heartbeat_ms_ > now ? next_heartbeat_ms_ - now : 0;
  }

  if (!election_deadline_ms_) {
    // Not a leader and no deadline yet - one Tick() away from arming.
    return options_.election_timeout_ms;
  }

  return *election_deadline_ms_ > now ? *election_deadline_ms_ - now : 0;
}

void RaftDriver::UpdateDeadline(u64 now)
{
  const RaftRole role = raft_.role();
  const u64 term = raft_.term();
  const u64 contact_ms = raft_.last_leader_contact_ms();

  const bool entered_leader =
      role == RaftRole::LEADER && observed_role_ != RaftRole::LEADER;
  // 观察到当前节点的Role/term/leader_contact_ms发生了变化，说明当前节点的选举计时器需要重新设置
  const bool reset_election_deadline = role != observed_role_ || term != observed_term_ ||
                     contact_ms != observed_contact_ms_;

  observed_role_ = role;
  observed_term_ = term;
  observed_contact_ms_ = contact_ms;

  if (role == RaftRole::LEADER) {
    // A leader campaigns for nobody.
    election_deadline_ms_.reset();

    if (entered_leader) {
      // BecomeLeader() already announced itself, so the next heartbeat is one
      // interval away rather than immediately due.
      // 设置一次够了，这里只是延迟下一次Tick发送心跳的时间，之后每次Tick都会发心跳
      next_heartbeat_ms_ = now + options_.heartbeat_interval_ms;
    }
    return;
  }

  // Any of the three changes means the instance just reset its own election
  // timer - it accepted a current-term AppendEntries/heartbeat, granted a
  // vote, or moved to a new term. Re-arm from now with a fresh random
  // timeout instead of re-deriving those rules from the message stream.
  if (!election_deadline_ms_ || reset_election_deadline) {
    election_deadline_ms_ = now + DrawElectionTimeoutMs();
  }
}

auto RaftDriver::DrawElectionTimeoutMs() -> u64
{
  // Uniform over [T, 2T), drawn afresh on every re-arm.
  std::uniform_int_distribution<u64> dist(options_.election_timeout_ms,
                                          options_.election_timeout_ms * 2 - 1);
  return dist(rng_);
}

} // namespace kraft
