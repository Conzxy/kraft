// SPDX-LICENSE-IDENTIFIER: BSD-2-Clause
#ifndef _KRAFT_H__
#define _KRAFT_H__

#include <cstdint>
#include <vector>
#include <functional>

//#include ""
#include "type.h"
#include "kraft.pb.h"
#include "macro.h"

namespace kraft {

using Request = Message;
using Response = Message;

enum : u64 {
  INVALID_TERM = (u64)-1,
  INVALID_INDEX = (u64)-1,
  INVALID_VOTED_FOR = (u64)-1,
  INVALID_ID = (u64)-1,
};

enum NodeFlag : u8 {
  NODE_NONE = 0,
  NODE_ACTIVE = 1,
  NODE_VOTING = (1 << 1),
  NODE_RECV_HARTBEAT = (1 << 2),
};

struct EntryMeta {
  u64 term = -1;
  u64 index = -1;
};

struct Node {
  u64 id = -1;
  void *ctx = nullptr; // context
  u8 flag = (NODE_ACTIVE | NODE_VOTING);

  Node() = default;

  // Just used for perfect forwarding.
  Node(u64 id_, void *ctx_, u8 flag_)
    : id(id_)
    , ctx(ctx_)
    , flag(flag_)
  {
  }

  KRAFT_INLINE bool IsActive() const noexcept
  {
    return flag & NODE_ACTIVE;
  }

  KRAFT_INLINE bool IsVoting() const noexcept
  {
    return flag & NODE_VOTING;
  }

  KRAFT_INLINE bool IsRecvHeartBeat() const noexcept
  {
    return flag & NODE_RECV_HARTBEAT;
  }
};

class Raft {
 public:
  enum class RaftState {
    FOLLOWER = 0,
    CANDIDATE,
    LEADER,
  };

  enum ErrorCode {
    E_OK = 0,
    E_NODE_NONEXISTS = 1,
    E_INVALID_REQUEST = 2,
    E_INVALID_RESPONSE = 3,
    E_INCORRECT_STATE = 4,
    E_NOT_ONLY_LEADER = 5,
    E_INCORRECT_TO_NODE,
    E_INCORRECT_FROM_NODE,
    E_TRUNCATE,
    E_UNKNONN,
    E_LOG_APPEND_ENTRY,
    E_LOG_GET_ENTRY,
    E_LOG_APPLY,
    E_ADD_PEER_NODE,
  };

  explicit Raft(size_t peer_node_num = 0);
  ~Raft() noexcept;

  KRAFT_DISABLE_COPY(Raft);

  /*--------------------------------------------------*/
  /* Node control                                     */
  /*--------------------------------------------------*/

  bool AddPeerNode(u64 id, void *user_ctx, void *conf_ctx, size_t conf_ctx_len);
  bool AddSelfNode(u64 id, void *user_ctx, void *conf_ctx, size_t conf_ctx_len);

  ErrorCode AskNodeId(void *user_ctx);
  ErrorCode RecvAskNodeResponse(Response &response, void *user_ctx);

  /*--------------------------------------------------*/
  /* Periodic operation                               */
  /*--------------------------------------------------*/

  void Periodic();
  void ElectionPeriodic();
  void HeartBeatPeriodic();

  /*--------------------------------------------------*/
  /* State machine                                    */
  /*--------------------------------------------------*/

  ErrorCode AppendLog(void const *data, size_t n);

  ErrorCode RecvVoteResponse(Response &rsp);
  ErrorCode RecvVoteRequest(Request &req);
  ErrorCode RecvHeartBeatResponse(Response &req);
  ErrorCode RecvHeartBeatRequest(Response &rep);
  ErrorCode RecvAppendEntriesResponse(Response &rep);
  ErrorCode RecvAppendEntriesRequest(Request &req);

  KRAFT_INLINE bool IsLeader() const noexcept
  {
    return state_ == RaftState::LEADER;
  }
  KRAFT_INLINE bool IsFollower() const noexcept
  {
    return state_ == RaftState::FOLLOWER;
  }
  KRAFT_INLINE bool IsCandidate() const noexcept
  {
    return state_ == RaftState::CANDIDATE;
  }

 private:
  /*--------------------------------------------------*/
  /* State getter                                     */
  /*--------------------------------------------------*/

  void BecomeLeader();
  void BecomeFollower();
  void BecomeCandidate();

  KRAFT_INLINE RaftState state() const noexcept
  {
    return state_;
  }

  KRAFT_INLINE u64 term() const noexcept
  {
    return persistent_state_.term();
  }

  KRAFT_INLINE u64 voted_for() const noexcept
  {
    return persistent_state_.voted_for();
  }

  KRAFT_INLINE u64 id() const noexcept
  {
    return persistent_state_.id();
  }

  KRAFT_INLINE bool IsMajorityVoted() const noexcept
  {
    return voted_num_ > ((peer_nodes_.size() + 1) >> 1);
  }

  KRAFT_INLINE u64 leader_id() const noexcept
  {
    return leader_id_;
  }

  KRAFT_INLINE void *leader_context() noexcept
  {
    if (leader_id_ == INVALID_INDEX) {
      return nullptr;
    }

    auto node_iter = peer_node_map_.find(leader_id_);
    KRAFT_ASSERT(node_iter != peer_node_map_.end(),
                 "The leader id must be a existed node");
    KRAFT_ASSERT(node_iter->first == peer_nodes_[node_iter->second].id, "");
    return peer_nodes_[node_iter->second].ctx;
  }

 private:
  // Should be call only once
  void SetId(u64 id);
  void SetVotedFor(u64 id);
  void SetTerm(u64 term);

  friend struct RaftImpl;
  struct RaftImpl;

  std::vector<Node> peer_nodes_;
  HashMap<u64, u64> peer_node_map_; // NodeId -> NodeIndex
  Node self_node_;

  // raft state
  RaftState state_;
  PersistentState persistent_state_;
  u64 entry_index_counter_;

  // For redirect
  u64 leader_id_;

  u64 commit_;
  u64 last_applied_;

  // candidate state
  u64 voted_num_;
  // leader state
  std::vector<u64> next_indices_;
  std::vector<u64> match_indices_;

  // callback
 public:
  std::function<void(char const *, size_t)> log_cb_;
  std::function<void()> flush_cb_;

  // Log storage engine interface.
  // I use callback as it is more flexible than virtual function.
  //
  // The performance of std::function<> and virtual function is likely
  // same(Release mode).
  // YOU don't care the performance problem.
  std::function<bool(Entry const &entry)> log_append_entry_cb_;
  std::function<bool(PersistentState const &state)> log_set_state_;
  std::function<EntryMeta()> log_get_last_entry_meta_cb_;
  std::function<u64(u64 index)> log_get_entry_meta_cb_;
  std::function<bool(u64 index)> log_truncate_after_cb_;
  std::function<bool(u64 index, Entry *entry)> log_get_entry_cb_;
  std::function<bool(Entry *)> log_apply_entry_cb_;

  using SendCallback =
      std::function<void(::google::protobuf::Message *, void *)>;
  SendCallback send_message_cb_;
};

} // namespace kraft

#endif
