// Jubatus: Online machine learning framework for distributed environment
// Copyright (C) 2011-2013 Preferred Infrastructure and Nippon Telegraph and Telephone Corporation.
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License version 2.1 as published by the Free Software Foundation.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

#include "push_mixer.hpp"

#include <string>
#include <utility>
#include <vector>
#include "jubatus/util/concurrent/lock.h"
#include "jubatus/util/lang/bind.h"
#include "jubatus/util/system/time_util.h"
#include "jubatus/core/framework/stream_writer.hpp"
#include "jubatus/core/framework/mixable.hpp"
#include "../../common/membership.hpp"
#include "../../common/mprpc/rpc_mclient.hpp"
#include "../../common/unique_lock.hpp"

using std::pair;
using std::string;
using std::vector;
using jubatus::util::concurrent::scoped_lock;
using jubatus::util::concurrent::scoped_rlock;
using jubatus::util::concurrent::scoped_wlock;
using jubatus::util::lang::bind;
using jubatus::util::lang::shared_ptr;
using jubatus::util::system::time::clock_time;
using jubatus::util::system::time::get_clock_time;

using jubatus::core::common::byte_buffer;
using jubatus::core::framework::stream_writer;
using jubatus::core::framework::packer;

namespace jubatus {
namespace server {
namespace framework {
namespace mixer {

namespace {

class push_communication_impl : public push_communication {
 public:
  push_communication_impl(
      const jubatus::util::lang::shared_ptr<common::lock_service>& zk,
      const string& type,
      const string& name,
      int timeout_sec);

  size_t update_members();
  size_t size() const;
  shared_ptr<common::try_lockable> create_lock();
  const vector<pair<string, int> >& servers_list() const;
  void pull(
      const pair<string, int>& server,
      const byte_buffer& arg,
      common::mprpc::rpc_result_object& result) const;
  void get_pull_argument(
      const pair<string, int>& server,
      common::mprpc::rpc_result_object& result) const;
  void push(const pair<string, int>& server, const byte_buffer& diff) const;

 private:
  vector<pair<string, int> > servers_;
  jubatus::util::lang::shared_ptr<common::lock_service> zk_;
  mutable jubatus::util::concurrent::mutex m_;  // saves servers_ and zk_
  const string type_;
  const string name_;
  const int timeout_sec_;
};

push_communication_impl::push_communication_impl(
    const jubatus::util::lang::shared_ptr<common::lock_service>& zk,
    const string& type,
    const string& name,
    int timeout_sec)
    : zk_(zk),
      type_(type),
      name_(name),
      timeout_sec_(timeout_sec) {
}

size_t push_communication_impl::update_members() {
  common::unique_lock lk(m_);
  common::get_all_nodes(*zk_, type_, name_, servers_);
  return servers_.size();
}

size_t push_communication_impl::size() const {
  common::unique_lock lk(m_);
  return servers_.size();
}

shared_ptr<common::try_lockable> push_communication_impl::create_lock() {
  // TODO(kumagi): push_mixer does not use zk_lock
  string path;
  common::unique_lock lk(m_);
  common::build_actor_path(path, type_, name_);
  return shared_ptr<common::try_lockable>(
      new common::lock_service_mutex(*zk_, path + "/master_lock"));
}

const vector<pair<string, int> >& push_communication_impl::servers_list()
    const {
  common::unique_lock lk(m_);
  return servers_;
}

void push_communication_impl::pull(
    const pair<string, int>& server,
    const byte_buffer& arg,
    common::mprpc::rpc_result_object& result) const {
  vector<pair<string, int> > servers;
  servers.push_back(server);

  // TODO(beam2d): to be replaced to new client with socket connection pooling
  common::mprpc::rpc_mclient client(servers, timeout_sec_);
  result = client.call("pull", arg);
}

void push_communication_impl::get_pull_argument(
  const pair<string, int>& server,
  common::mprpc::rpc_result_object& result) const {
  vector<pair<string, int> > servers;
  servers.push_back(server);

  // TODO(beam2d): to be replaced to new client with socket connection pooling
  common::mprpc::rpc_mclient client(servers, timeout_sec_);
  result = client.call("get_pull_argument", 0);
}

void push_communication_impl::push(
    const pair<string, int>& server,
    const byte_buffer& diff) const {
  vector<pair<string, int> > servers;
  servers.push_back(server);

  // TODO(beam2d): to be replaced to new client with socket connection pooling
  common::mprpc::rpc_mclient client(servers, timeout_sec_);
  client.call("push", diff);
}

}  // namespace

jubatus::util::lang::shared_ptr<push_communication> push_communication::create(
    const jubatus::util::lang::shared_ptr<common::lock_service>& zk,
    const string& type,
    const string& name,
    int timeout_sec) {
  return jubatus::util::lang::shared_ptr<push_communication_impl>(
      new push_communication_impl(zk, type, name, timeout_sec));
}

push_mixer::push_mixer(
    shared_ptr<push_communication> communication,
    jubatus::util::concurrent::rw_mutex& mutex,
    unsigned int count_threshold,
    unsigned int tick_threshold,
    const std::pair<std::string, int>& my_id)
    : communication_(communication),
      model_mutex_(mutex),
      count_threshold_(count_threshold),
      tick_threshold_(tick_threshold),
      my_id_(my_id),
      counter_(0),
      mix_count_(0),
      ticktime_(get_clock_time()),
      is_running_(false),
      t_(jubatus::util::lang::bind(&push_mixer::mixer_loop, this)) {
}

push_mixer::~push_mixer() {
  stop();
}

void push_mixer::register_api(rpc_server_t& server) {
  server.add<byte_buffer(msgpack::object)>(
      "pull", bind(&push_mixer::pull, this, jubatus::util::lang::_1));
  server.add<byte_buffer(int)>(  // NOLINT
      "get_pull_argument", bind(
          &push_mixer::get_pull_argument, this, jubatus::util::lang::_1));
  server.add<int(msgpack::object)>(
      "push", bind(&push_mixer::push, this, jubatus::util::lang::_1));
  server.add<bool(void)>(
      "do_mix", bind(&push_mixer::do_mix, this));
}

void push_mixer::set_driver(core::driver::driver_base* driver) {
  driver_ = driver;
}

void push_mixer::start() {
  scoped_lock lk(m_);
  if (!is_running_) {
    is_running_ = true;
    t_.start();
  }
}

void push_mixer::stop() {
  common::unique_lock lk(m_);
  if (is_running_) {
    is_running_ = false;
    lk.unlock();
    t_.join();
  }
}

bool push_mixer::do_mix() {
  {
    common::unique_lock lk(m_);
    counter_ = 0;
    ticktime_ = get_clock_time();
    lk.unlock();
  }
  try {
    LOG(INFO) << "forced to mix by user RPC";
    mix();
    return true;
  } catch (const jubatus::core::common::exception::jubatus_exception& e) {
    LOG(ERROR) << e.diagnostic_information(true);
  } catch (const std::exception& e) {
    LOG(WARNING) << "exception in mix: " << e.what();
  } catch (...) {
    LOG(ERROR) << "unexpected error";
  }
  return false;
}

void push_mixer::updated() {
  scoped_lock lk(m_);
  ++counter_;
  if (counter_ >= count_threshold_
      || get_clock_time() - ticktime_ > tick_threshold_) {
    c_.notify();  // FIXME: need sync here?
  }
}

void push_mixer::get_status(server_base::status_t& status) const {
  scoped_lock lk(m_);
  status["push_mixer.count"] =
    jubatus::util::lang::lexical_cast<string>(counter_);
  status["push_mixer.ticktime"] =
    jubatus::util::lang::lexical_cast<string>(ticktime_.sec);  // since last mix
}

void push_mixer::mixer_loop() {
  while (is_running_) {
    try {
      common::unique_lock lk(m_);
      if (!is_running_) {
        return;
      }

      c_.wait(m_, 0.5);
      clock_time new_ticktime = get_clock_time();
      if ((0 < count_threshold_ &&  counter_ >= count_threshold_)
          || (0 < tick_threshold_ && new_ticktime - ticktime_ > tick_threshold_)
          ) {
        DLOG(INFO) << "starting mix because of "
                   << (count_threshold_ <= counter_ ? "counter" : "tick_time")
                   << " threshold";
        counter_ = 0;
        ticktime_ = new_ticktime;

        lk.unlock();
        mix();
        DLOG(INFO) << ".... " << mix_count_ << "th mix done.";
      }
    } catch (const core::common::exception::jubatus_exception& e) {
      LOG(ERROR) << e.diagnostic_information(true);
    }
  }
}

void push_mixer::mix() {
  clock_time start = get_clock_time();
  size_t s_pull = 0, s_push = 0;

  size_t servers_size = communication_->update_members();
  if (servers_size == 0) {
    LOG(WARNING) << "no other server. ";
    return;
  } else {
    try {
      // call virtual function to select push candidate
      vector<const pair<string, int>*> candidates =
        filter_candidates(communication_->servers_list());

      for (size_t i = 0; i < candidates.size(); ++i) {
        const pair<string, int>& she = *candidates[i];

        // pull from her
        byte_buffer my_args = get_pull_argument(0);

        common::mprpc::rpc_result_object pull_result;
        communication_->pull(she, my_args, pull_result);
        msgpack::object her_diff = pull_result.response.front()();

        // pull from me
        common::mprpc::rpc_result_object args_result;
        communication_->get_pull_argument(she, args_result);
        msgpack::object her_args =
            args_result.response.front()();
        byte_buffer my_diff = pull(her_args);

        // push to her and me
        communication_->push(she, my_diff);
        push(her_diff);

        // count size
        s_pull += her_diff.via.raw.size;
        s_push += my_diff.size();
      }
      if (candidates.size() == 0U) {
        LOG(WARNING) << "no server selected";
      }
    } catch (const std::exception& e) {
      LOG(WARNING) << e.what() << " : mix failed";
      return;
    }
  }

  clock_time end = get_clock_time();
  LOG(INFO) << (end - start) << " time elapsed "
            << s_pull << " pulled  "
            << s_push << " pushed";
  mix_count_++;
}

byte_buffer push_mixer::pull(const msgpack::object& arg_obj) {
  if (arg_obj.type != msgpack::type::RAW) {
    throw msgpack::type_error();
  }
  msgpack::unpacked msg;
  msgpack::unpack(&msg, arg_obj.via.raw.ptr, arg_obj.via.raw.size);
  msgpack::object arg = msg.get();

  scoped_rlock lk_read(model_mutex_);
  scoped_lock lk(m_);

  core::framework::push_mixable* mixable =
    dynamic_cast<core::framework::push_mixable*>(driver_->get_mixable());

  msgpack::sbuffer sbuf;
  stream_writer<msgpack::sbuffer> st(sbuf);
  core::framework::jubatus_packer jp(st);
  packer pk(jp);

  mixable->pull(arg, pk);

  return byte_buffer(sbuf.data(), sbuf.size());
}

byte_buffer push_mixer::get_pull_argument(int dummy_arg) {
  scoped_rlock lk_read(model_mutex_);
  scoped_lock lk(m_);

  core::framework::push_mixable* mixable =
    dynamic_cast<core::framework::push_mixable*>(driver_->get_mixable());

  msgpack::sbuffer sbuf;
  stream_writer<msgpack::sbuffer> st(sbuf);
  core::framework::jubatus_packer jp(st);
  packer pk(jp);

  mixable->get_argument(pk);

  return byte_buffer(sbuf.data(), sbuf.size());
}

int push_mixer::push(const msgpack::object& diff_obj) {
  if (diff_obj.type != msgpack::type::RAW) {
    throw msgpack::type_error();
  }

  msgpack::unpacked msg;
  msgpack::unpack(&msg, diff_obj.via.raw.ptr, diff_obj.via.raw.size);
  msgpack::object diff = msg.get();

  scoped_wlock lk_write(model_mutex_);
  scoped_lock lk(m_);
  core::framework::push_mixable* mixable =
    dynamic_cast<core::framework::push_mixable*>(driver_->get_mixable());

  msgpack::sbuffer sbuf;
  stream_writer<msgpack::sbuffer> st(sbuf);
  core::framework::jubatus_packer jp(st);
  packer pk(jp);

  mixable->push(diff);

  counter_ = 0;
  ticktime_ = get_clock_time();
  return 0;
}

}  // namespace mixer
}  // namespace framework
}  // namespace server
}  // namespace jubatus