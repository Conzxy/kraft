// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
//
// End-to-end tests for RaftDriver: one real Raft instance per node, wired to a
// serialized in-memory "network" of kraft messages and to an in-memory log,
// driven by an injected clock. Nothing here reads the wall clock, so every
// election deadline is a known number of ticks away and the suite is
// deterministic.

#include "kraft/raft_driver.h"

#include <deque>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "kraft/error_code.h"

using namespace kraft;

namespace {

/*--------------------------------------------------*/
/* In-memory log storage                            */
/*--------------------------------------------------*/

// The storage layer a Raft instance reaches through its callbacks.
//
// entries_[k] holds log index k + 1, so index 0 is the sentinel every log
// implicitly starts with: term 0 and no entry.
class MemoryLog {
 public:
  void Install(Raft *raft)
  {
    raft->log_append_entry_cb_ = [this](Entry &entry) { return Append(entry); };
    raft->log_set_state_ = [this](PersistentState const &state) {
      return SetState(state);
    };
    raft->log_get_last_entry_meta_cb_ = [this] { return LastEntryMeta(); };
    raft->log_get_entry_meta_cb_ = [this](u64 index) {
      return EntryTerm(index);
    };
    raft->log_truncate_after_cb_ = [this](u64 index) {
      return TruncateAfter(index);
    };
    raft->log_get_entry_cb_ = [this](u64 index, Entry const **entry) {
      return GetEntry(index, entry);
    };
    raft->log_apply_entry_cb_ = [this](Entry const *entry) {
      return Apply(entry);
    };
  }

  /*----------------- storage interface -----------------*/

  // Every append is an append at the end: the entry the instance built has
  // already been given the index it must land on.
  bool Append(Entry &entry)
  {
    if (entry.index() != entries_.size() + 1) {
      ADD_FAILURE() << "appended entry index " << entry.index()
                    << " does not follow the last index " << entries_.size();
      return false;
    }
    entries_.push_back(entry);
    return true;
  }

  bool SetState(PersistentState const &state)
  {
    persisted_ = state;
    return true;
  }

  EntryMeta LastEntryMeta() const
  {
    if (entries_.empty()) {
      return EntryMeta{0, 0};
    }
    return EntryMeta{entries_.back().term(), entries_.back().index()};
  }

  u64 EntryTerm(u64 index) const
  {
    if (index == 0) {
      return 0; // the sentinel
    }
    if (index > entries_.size()) {
      return INVALID_TERM;
    }
    return entries_[index - 1].term();
  }

  // Keep everything through `index`, drop the rest.
  bool TruncateAfter(u64 index)
  {
    entries_.resize(index);
    return true;
  }

  bool GetEntry(u64 index, Entry const **entry)
  {
    if (index == 0 || index > entries_.size()) {
      return false;
    }
    *entry = &entries_[index - 1];
    return true;
  }

  bool Apply(Entry const *entry)
  {
    applied_.push_back(*entry); // a copy: the pointer only lives for the call
    applied_indices_.push_back(entry->index());
    return true;
  }

  /*----------------- test accessors -----------------*/

  size_t size() const noexcept { return entries_.size(); }

  Entry const &at(u64 index) const { return entries_[index - 1]; }

  std::string data_at(u64 index) const
  {
    return entries_[index - 1].has_data() ? entries_[index - 1].data()
                                          : std::string();
  }

  std::vector<u64> const &applied_indices() const noexcept
  {
    return applied_indices_;
  }

  std::vector<Entry> const &applied() const noexcept { return applied_; }

  PersistentState const &persisted() const noexcept { return persisted_; }

 private:
  std::vector<Entry> entries_;
  std::vector<u64> applied_indices_;
  std::vector<Entry> applied_;
  PersistentState persisted_;
};

/*--------------------------------------------------*/
/* A cluster of driven instances                    */
/*--------------------------------------------------*/

class TestCluster {
 public:
  struct Node {
    u64 id = 0;
    std::unique_ptr<MemoryLog> log;
    std::unique_ptr<Raft> raft;
    // Declared after raft, so it is destroyed before it: the driver holds a
    // reference to the instance.
    std::unique_ptr<RaftDriver> driver;
    std::vector<Raft::RaftRole> role_changes;
    std::vector<u64> commits;
  };

  // A message on the wire, kept serialized so that every hop exercises the
  // same encode/decode a real transport would.
  struct Envelope {
    std::string bytes;
    u64 from = 0;
    u64 to = 0;
  };

  u64 now_ms() const noexcept { return now_ms_; }

  Node *Find(u64 id)
  {
    for (auto &node : nodes_) {
      if (node->id == id) {
        return node.get();
      }
    }
    return nullptr;
  }

  Node &At(u64 id)
  {
    Node *node = Find(id);
    if (node == nullptr) {
      EXPECT_TRUE(false) << "no node with id " << id;
      return *nodes_.front();
    }
    return *node;
  }

  Node *AddNode(u64 id, RaftDriver::Options options)
  {
    nodes_.emplace_back(new Node());
    Node *node = nodes_.back().get();
    node->id = id;

    node->log.reset(new MemoryLog());
    node->raft.reset(new Raft(0));
    node->raft->log_cb_ = [node](char const *data, size_t n) {
      std::string s(data, n);
      printf("raft[%llu]: %s\n", (ull)node->id, s.c_str());
    };

    // One manual clock for the whole cluster: every node reads the same
    // variable, so time only moves where Run() moves it.
    node->raft->SetClock([this] { return now_ms_; });

    // AddSelfNode() rejects an id that is already a peer, so self comes first.
    EXPECT_TRUE(node->raft->AddSelfNode(id, node));
    node->log->Install(node->raft.get());

    // The ctx a Raft instance hands back to send_message_cb_ is always the ctx
    // of the peer the message is addressed to, so the destination is the whole
    // routing table.
    node->raft->send_message_cb_ = [this](::google::protobuf::Message *msg,
                                         void *user_ctx) {
      Post(*static_cast<Message *>(msg), static_cast<Node *>(user_ctx));
    };

    node->driver.reset(new RaftDriver(*node->raft, options));
    node->driver->OnRoleChange([node](Raft::RaftRole state) {
      node->role_changes.push_back(state);
    });
    node->driver->OnCommit([node](u64 index) { node->commits.push_back(index); });

    return node;
  }

  // Every node learns every other node. Two phases: a peer's ctx is the peer
  // itself, so all of them have to exist first.
  void ConnectAll()
  {
    for (auto &node : nodes_) {
      for (auto &peer : nodes_) {
        if (node == peer) {
          continue;
        }
        EXPECT_TRUE(node->raft->AddPeerNode(peer->id, peer.get()));
      }
    }
  }

  /*----------------- network -----------------*/

  void Isolate(u64 id) { isolated_.insert(id); }

  void Heal(u64 id) { isolated_.erase(id); }

  bool IsIsolated(u64 id) const { return isolated_.count(id) != 0; }

  size_t dropped() const noexcept { return dropped_; }

  void Post(Message const &msg, Node *dest)
  {
    if (dest == nullptr || IsIsolated(msg.from()) || IsIsolated(dest->id)) {
      ++dropped_; // a partition eats the message in both directions
      return;
    }

    Envelope env;
    env.from = msg.from();
    env.to = dest->id;
    // Serializing proves every required field of the flat envelope is set,
    // which is exactly what a real transport would demand.
    EXPECT_TRUE(msg.SerializeToString(&env.bytes));
    queue_.push_back(std::move(env));
  }

  void Deliver(Envelope const &env)
  {
    Node *dest = Find(env.to);
    if (dest == nullptr) {
      ADD_FAILURE() << "message addressed to unknown node " << env.to;
      return;
    }

    Message msg;
    if (!msg.ParseFromString(env.bytes)) {
      ADD_FAILURE() << "message from " << env.from << " did not parse";
      return;
    }

    switch (msg.type()) {
      case MSG_VOTE:
        ExpectOk(dest->raft->RecvVoteRequest(msg), "RecvVoteRequest");
        break;
      case MSG_VOTE_RES:
        ExpectOk(dest->raft->RecvVoteResponse(msg), "RecvVoteResponse");
        break;
      case MSG_AE:
        ExpectOk(dest->raft->RecvAppendEntriesRequest(msg),
                 "RecvAppendEntriesRequest");
        break;
      case MSG_AE_RES:
        ExpectOk(dest->raft->RecvAppendEntriesResponse(msg),
                 "RecvAppendEntriesResponse");
        break;
      case MSG_HEART_BEAT:
        ExpectOk(dest->raft->RecvHeartBeatRequest(msg), "RecvHeartBeatRequest");
        break;
      case MSG_HEART_BEAT_RES:
        ExpectOk(dest->raft->RecvHeartBeatResponse(msg),
                 "RecvHeartBeatResponse");
        break;
      default:
        ADD_FAILURE() << "unexpected message type " << msg.type();
        break;
    }
  }

  // Delivers until the network is quiet. A single delivery can make a node
  // answer or campaign, so the queue is drained to a fixed point.
  bool DeliverAll()
  {
    bool delivered = false;
    while (!queue_.empty()) {
      Envelope env = std::move(queue_.front());
      queue_.pop_front();
      Deliver(env);
      delivered = true;
    }
    return delivered;
  }

  void TickAll()
  {
    for (auto &node : nodes_) {
      node->driver->Tick();
    }
  }

  // Advances the clock in steps and lets the state machine settle after each
  // one. The driver derives its deadlines reactively, so a single large jump
  // could skip past one.
  void Run(u64 delta_ms, u64 step_ms = 10)
  {
    u64 remaining = delta_ms;
    while (remaining > 0) {
      const u64 step = KRAFT_MIN(step_ms, remaining);
      now_ms_ += step;
      remaining -= step;

      for (;;) {
        TickAll();
        if (!DeliverAll()) {
          break;
        }
      }
    }
  }

  // The only leader, or nullptr if there is none or more than one.
  Node *LeaderOf()
  {
    Node *leader = nullptr;
    for (auto &node : nodes_) {
      if (!node->raft->IsLeader()) {
        continue;
      }
      if (leader != nullptr) {
        return nullptr;
      }
      leader = node.get();
    }
    return leader;
  }

  /*----------------- error reporting -----------------*/

  // kerror aborts the process when an error is destroyed unchecked, and
  // info() is what marks it checked. Both helpers therefore read truthiness
  // first (operator bool() leaves an error unchecked) and call info() second.
  static void ExpectOk(Error const &err, char const *what)
  {
    if (!err) {
      return;
    }
    auto *info = err.info();
    ADD_FAILURE() << what << ": errcode = " << errcode
                  << (info != nullptr ? std::string(": ") + info->GetMessage()
                                      : std::string());
  }

  static void ExpectErr(Error const &err, int expected_errcode,
                        char const *what)
  {
    if (!err) {
      ADD_FAILURE() << what << ": expected a failure, got success";
      return;
    }
    auto *info = err.info();
    EXPECT_EQ(errcode, expected_errcode)
        << what << (info != nullptr ? std::string(": ") + info->GetMessage()
                                    : std::string());
  }

 private:
  u64 now_ms_ = 1000; // non-zero: 0 means "never contacted" to the instance
  std::deque<Envelope> queue_;
  std::set<u64> isolated_;
  size_t dropped_ = 0;
  std::vector<std::unique_ptr<Node>> nodes_;
};

// A three-node cluster whose first node times out long before the other two,
// so which node campaigns first is decided by the configuration rather than by
// the random seed.
RaftDriver::Options FastOptions(u64 id) { return RaftDriver::Options{60, 20, id}; }

RaftDriver::Options SlowOptions(u64 id) { return RaftDriver::Options{600, 200, id}; }

/*--------------------------------------------------*/
/* Tests                                            */
/*--------------------------------------------------*/

TEST(raft_driver, elects_one_leader_and_commits_its_no_op)
{
  TestCluster cluster;
  cluster.AddNode(1, FastOptions(1));
  cluster.AddNode(2, SlowOptions(2));
  cluster.AddNode(3, SlowOptions(3));
  cluster.ConnectAll();

  cluster.Run(400);

  TestCluster::Node *leader = cluster.LeaderOf();
  ASSERT_NE(leader, nullptr) << "expected exactly one leader";
  EXPECT_EQ(leader->id, 1u);

  for (u64 id = 1; id <= 3; ++id) {
    TestCluster::Node &node = cluster.At(id);
    // Term 1 everywhere: the election ran once, and the leader did not have to
    // campaign again.
    EXPECT_EQ(node.raft->term(), 1u) << "node " << id;
    EXPECT_EQ(node.raft->leader_id(), 1u) << "node " << id;
    // The no-op entry BecomeLeader() appends is committed on every node,
    // followers learning the index from a heartbeat.
    EXPECT_EQ(node.raft->commit_index(), 1u) << "node " << id;
    EXPECT_EQ(node.raft->last_applied(), 1u) << "node " << id;
    EXPECT_EQ(node.log->size(), 1u) << "node " << id;
    EXPECT_EQ(node.log->applied_indices(), std::vector<u64>{1}) << "node " << id;
    EXPECT_EQ(node.commits, std::vector<u64>{1}) << "node " << id;
    // The follower's persisted state is a real write through log_set_state_.
    EXPECT_EQ(node.log->persisted().term(), 1u) << "node " << id;
  }

  EXPECT_EQ(cluster.At(1).role_changes,
            (std::vector<Raft::RaftRole>{Raft::RaftRole::CANDIDATE,
                                          Raft::RaftRole::LEADER}));
  // A follower that never changes state never reports one.
  EXPECT_TRUE(cluster.At(2).role_changes.empty());
  EXPECT_TRUE(cluster.At(3).role_changes.empty());

  EXPECT_EQ(cluster.At(1).raft->voted_for(), 1u);
  EXPECT_EQ(cluster.At(2).raft->voted_for(), 1u);
}

TEST(raft_driver, heartbeats_keep_the_leader_in_power)
{
  TestCluster cluster;
  cluster.AddNode(1, FastOptions(1));
  cluster.AddNode(2, SlowOptions(2));
  cluster.AddNode(3, SlowOptions(3));
  cluster.ConnectAll();

  cluster.Run(3000);

  // Two followers whose base timeout is 600ms stay followers for 3s only
  // because the leader's 20ms heartbeats keep resetting their deadlines. Any
  // extra term here means a spurious election.
  ASSERT_NE(cluster.LeaderOf(), nullptr);
  EXPECT_EQ(cluster.LeaderOf()->id, 1u);
  for (u64 id = 1; id <= 3; ++id) {
    EXPECT_EQ(cluster.At(id).raft->term(), 1u) << "node " << id;
    EXPECT_EQ(cluster.At(id).raft->commit_index(), 1u) << "node " << id;
  }
  EXPECT_EQ(cluster.At(1).role_changes.size(), 2u); // candidate, then leader
  EXPECT_TRUE(cluster.At(2).role_changes.empty());
  EXPECT_TRUE(cluster.At(3).role_changes.empty());
}

TEST(raft_driver, replicates_an_entry_appended_by_the_leader)
{
  TestCluster cluster;
  cluster.AddNode(1, FastOptions(1));
  cluster.AddNode(2, SlowOptions(2));
  cluster.AddNode(3, SlowOptions(3));
  cluster.ConnectAll();
  cluster.Run(400);

  ASSERT_NE(cluster.LeaderOf(), nullptr);
  TestCluster::ExpectOk(cluster.At(1).raft->AppendLog("hello", 5), "AppendLog");
  cluster.Run(50);

  for (u64 id = 1; id <= 3; ++id) {
    TestCluster::Node &node = cluster.At(id);
    EXPECT_EQ(node.log->size(), 2u) << "node " << id;
    EXPECT_EQ(node.log->data_at(2), "hello") << "node " << id;
    EXPECT_EQ(node.raft->commit_index(), 2u) << "node " << id;
    EXPECT_EQ(node.raft->last_applied(), 2u) << "node " << id;
    EXPECT_EQ(node.log->applied_indices(), (std::vector<u64>{1, 2}))
        << "node " << id;
    EXPECT_EQ(node.commits, (std::vector<u64>{1, 2})) << "node " << id;
  }
  EXPECT_EQ(cluster.At(1).log->applied()[1].data(), "hello");
}

TEST(raft_driver, append_log_is_rejected_by_a_follower)
{
  TestCluster cluster;
  cluster.AddNode(1, FastOptions(1));
  cluster.AddNode(2, SlowOptions(2));
  cluster.ConnectAll();
  cluster.Run(400);

  ASSERT_NE(cluster.LeaderOf(), nullptr);
  TestCluster::ExpectErr(cluster.At(2).raft->AppendLog("nope", 4),
                         E_NOT_ONLY_LEADER, "follower AppendLog");
  EXPECT_EQ(cluster.At(2).log->size(), 1u);
  EXPECT_EQ(cluster.At(2).raft->commit_index(), 1u);
}

TEST(raft_driver, single_node_cluster_elects_itself_and_commits)
{
  TestCluster cluster;
  cluster.AddNode(1, FastOptions(1));

  cluster.Run(400);

  TestCluster::Node &node = cluster.At(1);
  ASSERT_TRUE(node.raft->IsLeader());
  EXPECT_EQ(node.raft->term(), 1u);
  EXPECT_EQ(node.raft->leader_id(), 1u);
  EXPECT_EQ(node.raft->commit_index(), 1u);
  EXPECT_EQ(node.raft->last_applied(), 1u);
  EXPECT_EQ(node.log->size(), 1u);
  EXPECT_EQ(node.commits, std::vector<u64>{1});
  // The committed entry is the leader's no-op: no payload to apply.
  ASSERT_EQ(node.log->applied().size(), 1u);
  EXPECT_EQ(node.log->applied()[0].type(), ENTRY_NORMAL);
  EXPECT_FALSE(node.log->applied()[0].has_data());

  // With no peers the entry replicates nowhere and commits immediately.
  TestCluster::ExpectOk(node.raft->AppendLog("x", 1), "AppendLog");
  cluster.Run(50);
  EXPECT_EQ(node.raft->commit_index(), 2u);
  EXPECT_EQ(node.log->applied_indices(), (std::vector<u64>{1, 2}));
}

TEST(raft_driver, follower_catches_up_after_a_partition)
{
  TestCluster cluster;
  cluster.AddNode(1, FastOptions(1));
  cluster.AddNode(2, SlowOptions(2));
  cluster.AddNode(3, SlowOptions(3));
  cluster.ConnectAll();
  cluster.Run(400);
  ASSERT_NE(cluster.LeaderOf(), nullptr);

  cluster.Isolate(3);
  TestCluster::ExpectOk(cluster.At(1).raft->AppendLog("a", 1), "AppendLog a");
  TestCluster::ExpectOk(cluster.At(1).raft->AppendLog("b", 1), "AppendLog b");
  cluster.Run(100);

  // Two of three replicas is a majority, so the leader commits without node 3.
  EXPECT_EQ(cluster.At(1).raft->commit_index(), 3u);
  EXPECT_EQ(cluster.At(1).log->size(), 3u);
  EXPECT_EQ(cluster.At(2).log->size(), 3u);
  EXPECT_EQ(cluster.At(2).raft->commit_index(), 3u);
  // Node 3 is behind and knows nothing about it: the leader never got a
  // rejection, it just got nothing at all.
  EXPECT_EQ(cluster.At(3).log->size(), 1u);
  EXPECT_EQ(cluster.At(3).raft->commit_index(), 1u);
  EXPECT_GT(cluster.dropped(), 0u);
  // The isolation window is far below node 3's 600ms base timeout, so it never
  // campaigns and the term does not move.
  EXPECT_EQ(cluster.At(3).raft->term(), 1u);
  EXPECT_EQ(cluster.At(3).role_changes.size(), 0u);

  cluster.Heal(3);
  cluster.Run(300);

  EXPECT_EQ(cluster.At(3).log->size(), 3u);
  EXPECT_EQ(cluster.At(3).log->data_at(2), "a");
  EXPECT_EQ(cluster.At(3).log->data_at(3), "b");
  EXPECT_EQ(cluster.At(3).raft->commit_index(), 3u);
  EXPECT_EQ(cluster.At(3).raft->last_applied(), 3u);
  EXPECT_EQ(cluster.At(3).log->applied_indices(), (std::vector<u64>{1, 2, 3}));
  EXPECT_EQ(cluster.At(3).raft->term(), 1u);
  EXPECT_EQ(cluster.LeaderOf()->id, 1u);
}

TEST(raft_driver, rejects_messages_that_fail_the_whitelist)
{
  TestCluster cluster;
  cluster.AddNode(1, FastOptions(1));
  cluster.AddNode(2, SlowOptions(2));
  cluster.ConnectAll();

  TestCluster::Node &node = cluster.At(2);
  const u64 term = node.raft->term();

  // Right shape, wrong message type: an AppendEntries entry point must not
  // accept anything else.
  Message wrong_type;
  wrong_type.set_type(MSG_NO_OP);
  wrong_type.set_term(term);
  wrong_type.set_from(1);
  wrong_type.set_to(2);
  TestCluster::ExpectErr(node.raft->RecvAppendEntriesRequest(wrong_type),
                         E_INCORRECT_MESSAGE_TYPE, "wrong type");

  // Right type, wrong destination.
  Message wrong_to = wrong_type;
  wrong_to.set_type(MSG_AE);
  wrong_to.set_to(99);
  TestCluster::ExpectErr(node.raft->RecvAppendEntriesRequest(wrong_to),
                         E_INCORRECT_TO_NODE, "wrong to");

  // Right type and destination, but from a node that is not a peer.
  Message unknown_from = wrong_type;
  unknown_from.set_type(MSG_AE);
  unknown_from.set_from(99);
  TestCluster::ExpectErr(node.raft->RecvAppendEntriesRequest(unknown_from),
                         E_INCORRECT_FROM_NODE, "unknown from");

  // None of the rejected messages left a trace.
  EXPECT_EQ(node.raft->term(), term);
  EXPECT_EQ(node.raft->leader_id(), INVALID_ID);
  EXPECT_EQ(node.raft->voted_for(), INVALID_VOTED_FOR);
  EXPECT_EQ(node.raft->commit_index(), 0u);
  EXPECT_EQ(node.log->size(), 0u);
  EXPECT_TRUE(node.role_changes.empty());
  EXPECT_TRUE(node.commits.empty());
}

} // namespace
