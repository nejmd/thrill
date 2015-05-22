/*******************************************************************************
 * c7a/net/channel_multiplexer.hpp
 *
 * Part of Project c7a.
 *
 *
 * This file has no license. Only Chuck Norris can compile it.
 ******************************************************************************/

#pragma once
#ifndef C7A_NET_CHANNEL_MULTIPLEXER_HEADER
#define C7A_NET_CHANNEL_MULTIPLEXER_HEADER

#include <c7a/net/net_dispatcher.hpp>
#include <c7a/net/net_group.hpp>
#include <c7a/net/channel.hpp>
#include <c7a/data/block_emitter.hpp>
#include <c7a/data/buffer_chain_manager.hpp>
#include <c7a/data/socket_target.hpp>

#include <memory>
#include <map>

namespace c7a {
namespace data {

struct BufferChain;

} // namespace data

namespace net {

//! \ingroup net
//! \{

typedef c7a::data::ChainId ChannelId;

//! Multiplexes virtual Connections on NetDispatcher
//!
//! A worker as a TCP conneciton to each other worker to exchange large amounts
//! of data. Since multiple exchanges can occur at the same time on this single
//! connection we use multiplexing. The slices are called Blocks and are
//! indicated by a \ref StreamBlockHeader. Multiple Blocks form a Stream on a
//! single TCP connection. The multi plexer multiplexes all streams on all
//! sockets.
//!
//! All sockets are polled for headers. As soon as the a header arrives it is
//! either attached to an existing channel or a new channel instance is
//! created.
//!
//! OpenChannel returns a set of emitters that can be used to emitt data to other workers.
class ChannelMultiplexer
{
public:
    ChannelMultiplexer(NetDispatcher& dispatcher)
        : group_(nullptr),
          dispatcher_(dispatcher) { }

    void Connect(std::shared_ptr<NetGroup> s) {
        group_ = s;
        for (size_t id = 0; id < group_->Size(); id++) {
            if (id == group_->MyRank()) continue;
            ExpectHeaderFrom(group_->Connection(id));
        }
    }

    //! Indicates if a channel exists with the given id
    //! Channels exist if they have been allocated before
    bool HasChannel(ChannelId id) {
        return channels_.find(id) != channels_.end();
    }

    //! Indicates if there is data for a certain channel
    //! Data exists as soon as either a channel has been allocated or data arrived
    //! on this worker with the given id
    bool HasDataOn(ChannelId id) {
        return chains_.Contains(id);
    }

    //! Returns the buffer chain that contains the data for the channel with the given id
    std::shared_ptr<data::BufferChain> AccessData(ChannelId id) {
        return chains_.Chain(id);
    }

    //! Allocate the next channel
    ChannelId AllocateNext() {
        return chains_.AllocateNext();
    }

    //! Creates emitters for each worker. Uses the given ChannelId
    //! Channels can be opened only once.
    //! Behaviour on multiple calls to OpenChannel is undefined.
    //! \param id the channel to use
    template <class T>
    std::vector<data::BlockEmitter<T> > OpenChannel(ChannelId id) {
        assert(group_ != nullptr);
        std::vector<data::BlockEmitter<T> > result;
        for (size_t worker_id = 0; worker_id < group_->Size(); worker_id++) {
            if (worker_id == group_->MyRank()) {
                auto closer = std::bind(&ChannelMultiplexer::CloseLoopbackStream, this, id);
                auto target = std::make_shared<data::LoopbackTarget>(chains_.Chain(id), closer);
                result.emplace_back(data::BlockEmitter<T>(target));
            }
            else {
                auto target = std::make_shared<data::SocketTarget>(
                    &dispatcher_,
                    &(group_->Connection(worker_id)),
                    id);

                result.emplace_back(data::BlockEmitter<T>(target));
            }
        }
        return result;
    }

    //! Closes all client connections
    //!
    //! Requires new call to Connect() afterwards
    void Close() {
        group_->Close();
    }

private:
    static const bool debug = false;
    typedef std::shared_ptr<Channel> ChannelPtr;

    //! Channels have an ID in block headers
    std::map<int, ChannelPtr> channels_;
    data::BufferChainManager chains_;

    //Hols NetConnections for outgoing Channels
    std::shared_ptr<NetGroup> group_;

    NetDispatcher& dispatcher_;

    //! expects the next header from a socket and passes to ReadFirstHeaderPartFrom
    void ExpectHeaderFrom(NetConnection& s) {
        auto expected_size = sizeof(StreamBlockHeader::expected_bytes) + sizeof(StreamBlockHeader::channel_id);
        auto callback = std::bind(&ChannelMultiplexer::ReadFirstHeaderPartFrom, this, std::placeholders::_1, std::placeholders::_2);
        dispatcher_.AsyncRead(s, expected_size, callback);
    }

    //! Nasty hack because LoopbackTarget cannot send a end-of-stream header
    void CloseLoopbackStream(int id) {
        GetOrCreateChannel(id)->CloseLoopback();
    }

    ChannelPtr GetOrCreateChannel(int id) {
        ChannelPtr channel;
        if (!HasChannel(id)) {
            //create buffer chain target if it does not exist
            if (!chains_.Contains(id))
                chains_.Allocate(id);
            auto targetChain = chains_.Chain(id);

            //build params for Channel ctor
            auto callback = std::bind(&ChannelMultiplexer::ExpectHeaderFrom, this, std::placeholders::_1);
            auto expected_peers = group_->Size();
            channel = std::make_shared<Channel>(dispatcher_, callback, id, expected_peers, targetChain);
            channels_.insert(std::make_pair(id, channel));
        }
        else {
            channel = channels_[id];
        }
        return channel;
    }

    //! parses the channel id from a header and passes it to an existing
    //! channel or creates a new channel
    void ReadFirstHeaderPartFrom(
        NetConnection& s, const Buffer& buffer) {
        struct StreamBlockHeader header;
        header.ParseHeader(buffer.ToString());

        auto id = header.channel_id;
        ChannelPtr channel = GetOrCreateChannel(id);
        channel->PickupStream(s, header);
    }
};

} // namespace net
} // namespace c7a

#endif // !C7A_NET_CHANNEL_MULTIPLEXER_HEADER

/******************************************************************************/
