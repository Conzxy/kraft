// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#ifndef _KRAFT_RAFT_DRIVER_H__
#define _KRAFT_RAFT_DRIVER_H__

#include <functional>
#include <optional>
#include <random>

#include "kraft/kraft.h"
#include "kraft/macro.h"
#include "kraft/type.h"

namespace kraft {

/**
 * \brief Drives a Raft instance's timers, so the host does not have to.
 *
 * A Raft instance is a pure state machine: it never starts a timer by
 * itself, it only exposes Periodic()/ElectionPeriodic()/HeartBeatPeriodic()
 * and the host has to decide when to call which. That decision is the
 * election timeout and the heartbeat interval, and it is exactly the part
 * every host gets wrong, because it has to be re-derived from the Raft
 * rules (a heartbeat or a granted vote resets the election timer, a term
 * change restarts it, ...).
 *
 * This class owns those timers. The host keeps providing the clock - the
 * driver never reads the wall clock, it only asks the instance - and then
 * calls Tick() whenever its event loop has nothing better to do, waiting
 * Tick()'s return value before calling again:
 *
 * \code
 *   // One loop, one thread. Now() is the host's own monotonic clock.
 *   driver.SetClock([this] { return event_loop.NowMs(); });
 *   driver.OnRoleChange([this](Raft::RaftState s) { OnLeaderChanged(s); });
 *   driver.OnCommit([this](u64 index) { WakeUpApplier(index); });
 *
 *   for (;;) {
 *     event_loop.WaitFor(TimerQueue, driver.Tick());
 *     // ... deliver incoming messages to raft.Recv*()
 *   }
 * \endcode
 *
 * With the driver in place the host implements three storage methods, one
 * send method and one clock, and never guesses a cadence.
 *
 * \warning The instance must outlive the driver.
 * \warning The host notification callbacks may fire from inside Recv*(), not
 * only from Tick(), so the host must not call back into the instance (a new
 * Recv* or AppendLog) from them.
 */
class RaftDriver {
 public:
  using RaftRole = Raft::RaftRole;

  struct Options {
    // Base election timeout, in ms. The actual timeout is drawn from
    // [election_timeout_ms, 2 * election_timeout_ms) every time the driver
    // re-arms the timer, so that two followers that last heard from the
    // leader at the same moment do not start an election in the same tick.
    // A timeout of 0 is clamped to 1.
    u64 election_timeout_ms = 1000;

    // How often the leader announces itself. Must be well below
    // election_timeout_ms, or a follower will time the leader out.
    // A value of 0 is clamped to 1.
    u64 heartbeat_interval_ms = 100;

    // Seed of the generator behind the randomized election timeout. The
    // default 0 keeps a single-node run reproducible; tests pin it so that
    // every election deadline is a known number of ticks away.
    u64 random_seed = 0;
  };

  RaftDriver(Raft &raft, Options options);
  explicit RaftDriver(Raft &raft)
    : RaftDriver(raft, Options{}) 
  {}

  // Detaches from the instance: the notification callbacks this driver
  // installed (and any the host had installed before) are swapped back out.
  ~RaftDriver() noexcept;

  KRAFT_DISABLE_MOVE(RaftDriver);
  KRAFT_DISABLE_COPY(RaftDriver);

  /*--------------------------------------------------*/
  /* Host-provided environment                        */
  /*--------------------------------------------------*/

  // The host's time source, forwarded to the instance. The driver reads time
  // only through the instance, so this is the single place a clock is set.
  void SetClock(Clock clock) noexcept { raft_.SetClock(std::move(clock)); }

  /*--------------------------------------------------*/
  /* Host notification                                */
  /*--------------------------------------------------*/

  // Called with the new state after it changed, so state()/leader_id()
  // inside the callback already observe it. Replaces polling IsLeader().
  void OnRoleChange(std::function<void(RaftRole)> cb)
  {
    on_role_change_ = std::move(cb);
  }

  // Called with the new commit index after it advanced. Replaces polling
  // commit_index(). Application itself still happens in Tick(), so an
  // implementation can simply record the index and return.
  void OnCommit(std::function<void(u64)> cb) { on_commit_ = std::move(cb); }

  /*--------------------------------------------------*/
  /* Driving                                          */
  /*--------------------------------------------------*/

  /**
   * \brief Advance the timers, then report when to call again.
   *
   * Runs the state machine once: applies committed entries, replicates to
   * the peers if this instance is the leader and the heartbeat is due, or
   * starts an election if it is not and the election timeout is due.
   *
   * \return The number of milliseconds until the next call: at least 1, so a
   * host doing `WaitFor(TimerQueue, driver.Tick())` never spins. By the time
   * the trailing UpdateDeadline() runs, whatever happened above has left a
   * deadline in the future - a leader one heartbeat interval away, a follower
   * a fresh randomized election timeout away, and an instance that is not
   * ready a whole election timeout away. The 0 NextTimeoutMs() can return is
   * only observable from outside, once a deadline has actually passed.
   *
   * \note A host that ignores the return value and polls invokes no more work
   * than the return value would have scheduled; the value is purely an
   * optimisation, not a contract on the state machine.
   */
  u64 Tick();

  /**
   * \brief Milliseconds until the next Tick() that has something to do.
   *
   * 0 means a deadline has already passed.
   */
  u64 NextTimeoutMs() const noexcept;

  /*--------------------------------------------------*/
  /* Accessors                                        */
  /*--------------------------------------------------*/

  // The instance is still driven through its own interface: Recv*() on an
  // incoming message, AppendLog() on a client request.
  Raft &raft() noexcept { return raft_; }
  Raft const &raft() const noexcept { return raft_; }

  Options const &options() const noexcept { return options_; }

 private:
  // Re-derives the deadlines from what the instance looks like right now.
  // It deliberately does not re-implement the Raft rules: it only reacts to
  // the instance's own record of when the election timer was last reset
  // (last_leader_contact_ms()) and to its state/term changes.
  void UpdateDeadline(u64 now);

  // A fresh randomized election timeout, uniform in [T, 2T).
  u64 DrawElectionTimeoutMs();

 private:
  Raft &raft_;
  Options options_;
  std::mt19937_64 rng_;

  std::function<void(RaftRole)> on_role_change_;
  std::function<void(u64)> on_commit_;

  // What the instance's notification callbacks held before this driver
  // installed its own, so the destructor can put them back.
  std::function<void(RaftRole)> prev_on_role_change_;
  std::function<void(u64)> prev_on_commit_;

  // Election timer. Empty while this instance is the leader: a leader
  // campaigns for nobody, so it has no election deadline. For a candidate or
  // follower, UpdateDeadline() arms it with a fresh randomized deadline;
  // an empty value therefore means "not armed yet", and NextTimeoutMs()
  // reports that as one whole election timeout away.
  std::optional<u64> election_deadline_ms_;

  u64 next_heartbeat_ms_ = 0;

  // The last observed (state, term, contact) triple, used to notice that the
  // instance reset its own election timer.
  // 用于检测是否重置election deadline的元数据
  RaftRole observed_role_ = RaftRole::FOLLOWER;
  u64 observed_term_ = INVALID_TERM;
  u64 observed_contact_ms_ = 0;
};

} // namespace kraft

#endif
