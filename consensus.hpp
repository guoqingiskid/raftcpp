#pragma once
#include <mutex>
#include <random>

#include "entity.h"
#include "common.h"
#include "message_bus.hpp"
#include "mem_log.hpp"

namespace raftcpp {
	static std::default_random_engine g_generator;

	class consensus {
	public:
		consensus(int host_id, int peers_num) : host_id_(host_id), peers_num_(peers_num), 
			bus_(message_bus::get()), log_(mem_log_t::get()) {
			init();
		}

		void init() {
			bus_.subscribe<msg_election_timeout>(&consensus::election_timeout, this);
			bus_.subscribe<msg_vote_timeout>(&consensus::vote_timeout, this);
			bus_.subscribe<msg_heartbeat_timeout>(&consensus::start_heartbeat, this);

			bus_.subscribe<msg_pre_request_vote>(&consensus::pre_request_vote, this);
			bus_.subscribe<msg_request_vote>(&consensus::request_vote, this);
			bus_.subscribe<msg_heartbeat>(&consensus::heartbeat, this);
			bus_.subscribe<msg_append_entry>(&consensus::append_entry, this);

			bus_.subscribe<msg_handle_response_of_request_vote>(&consensus::handle_response_of_request_vote, this);
			bus_.subscribe<msg_handle_response_of_request_heartbeat>(&consensus::handle_response_of_request_heartbeat, this);
			bus_.subscribe<msg_handle_response_of_append_entry>(&consensus::handle_response_of_append_entry, this);

			state_ = State::FOLLOWER;
			print("start pre_vote timer\n");
			restart_election_timer(ELECTION_TIMEOUT);
		}

		/********** rpc service start *********/
		response_vote pre_request_vote(request_vote_t args) {
			std::unique_lock<std::mutex> lock(mtx_);
			response_vote vote = {};
			vote.term = current_term_;

			if (args.term < current_term_) {
				return vote;
			}

			if (check_state()) {
				return vote;
			}

			auto last_index = log_.last_index();
			auto last_term = log_.get_term(last_index);
			bool log_ok = (args.last_log_term > last_term ||
				args.last_log_term == last_term &&
				args.last_log_idx >= last_index);
			if (!log_ok) {
				vote.vote_granted = false;
			}
			else {
				vote.vote_granted = true;
			}

			return vote;
		}

		response_vote request_vote(request_vote_t args) {
			std::unique_lock<std::mutex> lock(mtx_);
			response_vote vote{};
			vote.vote_granted = false;
			do {
				if (args.term < current_term_) {
					break;
				}

				if (args.term > current_term_) {
					if (check_state()) {
						break;
					}
					
					step_down_follower(args.term);
				}

				if (args.term == 0|| (vote_for_ != -1 && vote_for_ != args.from)) {
					break;
				}

				auto last_index = log_.last_index();
				auto last_term = log_.get_term(last_index);
				bool log_ok = (args.last_log_term > last_term ||
					args.last_log_term == last_term &&
					args.last_log_idx >= last_index);

				if ((vote_for_ == -1|| vote_for_==args.from) && log_ok) {
					vote_for_ = args.from;
					vote.vote_granted = true;
					step_down_follower(args.term);
				}

				if (!log_ok)
					vote.vote_granted = false;
			} while (0);

			vote.term = current_term_;
			return vote;
		}

		void receive_heartbeat_handler(req_heartbeat& args, res_heartbeat& res) {
			step_down_follower(current_term_);
			reset_leader_id(args.from);
			leader_commit_index_ = std::min(args.leader_commit_index, mem_log_t::get().last_index());
			res.term = current_term_;
		}

		res_heartbeat heartbeat(req_heartbeat args) {
			std::unique_lock<std::mutex> lock(mtx_);
			print("recieved heartbeat\n");
			res_heartbeat hb{ host_id_, current_term_ };
			if (args.term < current_term_) {
				return hb;
			}

			if (state_ == State::FOLLOWER) {
				receive_heartbeat_handler(args, hb);
				return hb;
			}
			else if (state_ == State::CANDIDATE) {
				receive_heartbeat_handler(args, hb);
				return hb;
			}
			else if (state_ == State::LEADER) {
				if (args.term > current_term_) {
					receive_heartbeat_handler(args, hb);
					return hb;
				}
			}
			
			return hb;
		}

		res_append_entry append_entry(req_append_entry args) {
			std::cout << "enter append_entry" << std::endl;
			//std::cout << "node {id =" << host_id_ << "} handle append entry request from node { id=" << args.from << "}\n";
			//req_id---log_index in map 
			res_append_entry res;
			res.term = current_term_;
			res.from = host_id_;

			if (args.term > current_term_) {
				step_down_follower(args.term);
				reset_leader_id(args.from);
			}

			res.term = current_term_;
			if (args.prev_log_index < leader_commit_index_) {
				res.last_log_index = leader_commit_index_;
				return res;
			}
			if (args.prev_log_term != log_.get_term(args.prev_log_index)) {
				res.reject_hint = log_.last_index();
				return res;
			}
			uint64_t conflict_index = log_.find_conflict(args.entries);
			if (conflict_index == 0) {
				res.reject = true;
				res.reject_hint = log_.last_index();
				return res;
			}
			else {
				assert(conflict_index > leader_commit_index_);

				auto pos = std::find_if(args.entries.begin(), args.entries.end(), [conflict_index](const entry_t& e) {return e.index == conflict_index; });
				std::vector<entry_t> v(pos, args.entries.end());
				log_.append_may_truncate(v);
			}
			leader_commit_index_ = std::min(args.leader_commit_index, log_.last_index());
			res.last_log_index = log_.last_index();
			res.reject = false;
			return res;
		}
		/********** rpc service end *********/

		State state() {
			return state_;
		}

		uint64_t commit_index() {
			return leader_commit_index_;
		}

		uint64_t current_term() {
			return current_term_;
		}

		void set_commit_index(uint64_t comm_idx) {
			leader_commit_index_ = comm_idx;
		}

		void restart_election_timer(int timeout) {
			election_timeout_ = false;
			bus_.send_msg<msg_restart_election_timer>(timeout);
		}

		void election_timeout() {
			print("election timeout\n");
			std::unique_lock<std::mutex> lock(mtx_);
			election_timeout_ = true;
			assert(state_ == State::FOLLOWER);
			//if no other peers form configure, just me, become leader
			if (peers_num_==0) {
				become_candidate();
				return;
			}
			reset_leader_id();
			pre_vote();
		}

		void pre_vote() {
			request_vote_t vote{};
			vote.term = current_term_ + 1;
			vote.last_log_idx = log_.last_index();

			start_vote(true);
			print("start pre_vote timer\n");
			restart_election_timer(ELECTION_TIMEOUT);
		}

		void become_candidate() {
			bus_.send_msg<msg_cancel_election_timer>();

			print("become candidate\n");
			reset_leader_id();
			state_ = State::CANDIDATE;
			current_term_++;
			vote_for_ = host_id_;
			print("start vote timer\n");
			bus_.send_msg<msg_restart_vote_timer>();

			//const LogId last_log_id = _log_manager->last_log_id(true);

			start_vote();
		}

		void start_vote(bool is_pre_vote = false){
			is_pre_vote ? print("start pre_vote\n") : print("start vote\n");
			auto counter = std::make_shared<int>(1);

			handle_majority(*counter, is_pre_vote);

			request_vote_t vote{};
			uint64_t term = current_term_;
			vote.term = is_pre_vote ? current_term_ + 1 : current_term_;
			vote.last_log_idx = log_.last_index(); 
			vote.last_log_term = log_.get_term(vote.last_log_idx);
			vote.from = host_id_;

			bus_.send_msg<msg_broadcast_request_vote>(is_pre_vote, term, counter, vote);
		}

		void vote_timeout() {
			std::unique_lock<std::mutex> lock(mtx_);
			if (state_ != State::CANDIDATE) {
				return;
			}

			step_down_follower(current_term_);
		}

		void step_down_follower(uint64_t term) {
			if (state_ == State::CANDIDATE) {
				print("stop vote timer\n");
				bus_.send_msg<msg_cancel_vote_timer>();
			}
			else if (state_ == State::LEADER) {
				bus_.send_msg<msg_cancel_heartbeat_timer>();
			}

			print("become follower\n");
			if (term > current_term_) {
				vote_for_ = -1;
				current_term_ = term;
			}
			
			state_ = State::FOLLOWER;
			print("start pre_vote timer\n");
			restart_election_timer(random_election());
			reset_leader_id();
		}

		void handle_response_of_request_vote(response_vote& resp_vote, uint64_t term, std::shared_ptr<int> counter, bool is_pre_vote) {
			std::unique_lock<std::mutex> lock(mtx_);
			if (state_ != (is_pre_vote ? State::FOLLOWER : State::CANDIDATE)) {
				return;
			}

			if (current_term_ != term) {
				return;
			}

			if (resp_vote.term > current_term_) {
				step_down_follower(resp_vote.term);
				return;
			}

			if (resp_vote.vote_granted) {
				(*counter)++;
			}

			handle_majority(*counter, is_pre_vote);
		}

		void handle_response_of_request_heartbeat(res_heartbeat resp_entry) {
			//todo progress
		}

		void handle_response_of_append_entry() {

		}

		void handle_majority(int count, bool is_pre_vote) {
			if (count > (peers_num_ + 1) / 2) {
				if (is_pre_vote) {
					print("get major prevote\n");
					become_candidate();
				}
				else {
					print("get major vote\n");
					become_leader();
				}
			}
		}

		void become_leader() {
			if (state_ != State::CANDIDATE) {
				return;
			}

			print("become leader\n");
			bus_.send_msg<msg_cancel_vote_timer>();
			state_ = State::LEADER;
			reset_leader_id(host_id_);
			bus_.send_msg<msg_restart_heartbeat_timer>();
		}

		void start_heartbeat() {
			std::unique_lock<std::mutex> lock(mtx_);
			req_heartbeat entry{};
			entry.from = host_id_;
			entry.term = current_term_;
			entry.leader_commit_index = leader_commit_index_;
			//entry.prev_log_index = todo
			//entry.prev_log_term = 

			bus_.send_msg<msg_broadcast_request_heartbeat>(entry);
			bus_.send_msg<msg_restart_heartbeat_timer>();
		}

		void reset_leader_id(int id = -1) {
			leader_id_ = id;
		}

		bool check_state() {
			if (state_ == State::LEADER) {
				if (active_num() + 1 > (peers_num_ + 1) / 2) {
					return true;
				}
			}
			else if (state_ == State::FOLLOWER) {
				if (leader_id_ != -1 && !election_timeout_) {
					return true;
				}
			}

			return false;
		}

		int active_num() {
			return bus_.send_msg<msg_active_num, int>();
		}

		template<typename T>
		T rand(T n) {
			std::uniform_int_distribution<T> dis(0, n - 1);
			return dis(g_generator);
		}

		int random_election() {
			return ELECTION_TIMEOUT + rand(ELECTION_TIMEOUT);
		}

		int leader_id() {
			return leader_id_;
		}

	private:
		State state_;
		int leader_id_ = -1;
		uint64_t current_term_ = 0;
		uint64_t leader_commit_index_ = 0;
		int vote_for_ = -1;
		bool election_timeout_ = false;

		int host_id_;
		int peers_num_ = 0;
		message_bus& bus_;
		mem_log_t& log_;
		std::mutex mtx_;
	};
}

