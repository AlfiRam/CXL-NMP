/*
 * Copyright (c) 2018 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __MEM_INTEGRITY_VERIFIER_HH__
#define __MEM_INTEGRITY_VERIFIER_HH__

#include <queue>

#include "enums/IntegrityAllocationMode.hh"
#include "enums/IntegrityTreeType.hh"
#include "enums/MetadataCacheType.hh"
#include "mem/cache/metadata_cache.hh"
#include "mem/qport.hh"
#include "params/AbstractIntegrityVerifier.hh"
#include "params/IntegrityVerifier.hh"
#include "sim/clocked_object.hh"
#include "sim/system.hh"

namespace gem5
{

/**
 * This abstract component provides a mechanism to perform integrity
 * verification. It can be spliced between arbitrary ports of the memory
 * system and delays packets that pass through it.
 *
 * Specialisations of this abstract class should override at least one
 * of delayReq, delayResp, deleySnoopReq, delaySnoopResp. These
 * methods receive a PacketPtr as their argument and return a delay in
 * Ticks. The base class implements an infinite buffer to hold delayed
 * packets until they are ready. The intention is to use this
 * component for rapid prototyping of other memory system components
 * that introduce a packet processing delays.
 */
class AbstractIntegrityVerifier : public ClockedObject
{
  public:
    PARAMS(AbstractIntegrityVerifier);

    AbstractIntegrityVerifier(const Params &p);

    ~AbstractIntegrityVerifier();

    void init() override;

  protected:
    /**
     * The simulated integrity tree.
     */
    AbstractIntegrityTree *integrityTree;

    enums::IntegrityTreeType integrityTreeType;

    enums::IntegrityAllocationMode integrityAllocationMode;

    /**
     * Used if a new metadata request is generated.
     */
    RequestorID _requestorId;

    /**
     * Time (in cycles) to complete hashing.
     */
    Cycles integrityHashingLatency;

    /**
     * Time (in cycles) to complete an XOR.
     */
    Cycles xorLatency;

    /**
     * Keep a pointer to the system to allow querying memory properties.
     */
    System *system;

    /**
     * Ranges are expected to be constructed where all ranges in each list are
     * disjoint, and sorted by starting address.
     *
     * Additionally, the "full" ranges and the "OS" ranges must specifically
     * be constructed such that the full ranges are identical to the OS ranges,
     * but there may be additional space at the end of the last full range or
     * additional ranges in the full range list.
     */

    AddrRangeList dramFullRanges;
    AddrRangeList dramOsRanges;
    AddrRangeList dramIntegrityRanges;
    AddrRangeList cxlFullRanges;
    AddrRangeList cxlOsRanges;
    AddrRangeList cxlIntegrityRanges;

    /**
     * Convienence range list for all integrity ranges.
     */
    AddrRangeList integrityRanges;

    /**
     * §6.2 range-keyed subtree handoff. handoffRangeStart/Size come from
     * params; handoffActive is (size > 0). When active, the access-address
     * range is mapped (at init()) to the contiguous MAC-leaf-index range
     * [handoffMacStart, handoffMacEnd): the integrity walk short-circuits at
     * any MAC node in that range, re-rooting the handed-off region instead of
     * climbing to the global root (node 0). No tree node id is involved.
     */
    Addr handoffRangeStart;
    Addr handoffRangeSize;
    bool handoffActive;
    size_t handoffMacStart;
    size_t handoffMacEnd;


    /**
     * Store the outstanding hash/pad generation while we wait for the
     * hash/pad to complete. Once it is done, we can use it to XOR with some
     * data, either to encrypt or decrypt, then if everything passes, the
     * original packet can be properly forwarded to memory/LLC (depending on
     * the type of operation being performed).
     */
    std::unordered_set<RequestPtr> outstandingIntegrityHashes;

    /**
     * Store the outsanding XOR computation while it completes.
     */
    std::unordered_set<RequestPtr> outstandingXors;

    /**
     * Store the outstanding request pkts that have not been verified yet. This
     * may or may not be used based on whether there should be a lazy
     * verification strategy (start speculatively using the data received
     * before verification has completed).
     */
    std::unordered_set<PacketPtr> outstandingIntegrityVerification;

    /**
     * Store the pending metadata evictions coming from the cache. This may
     * be used to handle certain cases where data is evicted then
     * (re-)accessed.
     */
    std::unordered_set<uint64_t> pendingMetadataEvictions;

    const uint64_t maxEncQueueSize = 1024;


    /**
     * Return if this integrity verifier has valid DRAM and CXL ranges stored,
     * based on the integrity allocation mode.
     */
    bool hasValidRanges();

    /**
     * Return if the size of the integrity tree is at least the size of the
     * expected memory to use for integrity data.
     */
    bool treeSizeValid();

    /**
     * Returns true if this packet should be handled by integrity
     * verification.
     */
    bool needsVerification(PacketPtr pkt);


    // Port interface

    Port &getPort(const std::string &if_name,
                  PortID idx=InvalidPortID) override;

    bool trySatisfyFunctional(PacketPtr pkt);

    /// CPU-side port. Receive requests from CPU end send responses to CPU.
    class ResponsePort : public QueuedResponsePort
    {
      public:
        ResponsePort(const std::string &_name,
          AbstractIntegrityVerifier &_parent);

      protected:
        Tick recvAtomic(PacketPtr pkt) override;
        bool recvTimingReq(PacketPtr pkt) override;
        void recvFunctional(PacketPtr pkt) override;
        bool recvTimingSnoopResp(PacketPtr pkt) override;

        AddrRangeList getAddrRanges() const override {
            return parent.requestPort.getAddrRanges();
        }

        bool tryTiming(PacketPtr pkt) override { return true; }

      private:
        AbstractIntegrityVerifier& parent;

    };

    /**
     * Process a request. This will either involve moving to verification
     * or forwarding the request to memory.
     */
    bool processReq(PacketPtr pkt);

    /// Memory-side port. Send requests to memory.
    class RequestPort : public QueuedRequestPort
    {
      public:
        RequestPort(const std::string &_name,
          AbstractIntegrityVerifier &_parent);

      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvFunctionalSnoop(PacketPtr pkt) override;
        Tick recvAtomicSnoop(PacketPtr pkt) override;
        void recvTimingSnoopReq(PacketPtr pkt) override;

        void recvRangeChange() override {
            parent.responsePort.sendRangeChange();
        }

        bool isSnooping() const override {
            return parent.responsePort.isSnooping();
        }

      private:
        AbstractIntegrityVerifier& parent;
    };

    /**
     * Process a response. This will either involve moving to verification
     * or passing on to the CPU if no verification is needed.
     */
    bool processResp(PacketPtr pkt);

    // Port that receives requests from the metadata cache and sends responses.
    class MetadataResponsePort : public QueuedResponsePort
    {
      public:
        MetadataResponsePort(const std::string &_name,
          AbstractIntegrityVerifier &_parent);

      protected:
        Tick recvAtomic(PacketPtr pkt) override {
          fatal("Unimplemented");
          return 0;
        }
        bool recvTimingReq(PacketPtr pkt) override;
        void recvFunctional(PacketPtr pkt) override {
          fatal("Unimplemented");
        }
        bool recvTimingSnoopResp(PacketPtr pkt) override {
          fatal("Unimplemented");
          return true;
        }

        AddrRangeList getAddrRanges() const override {
            // return parent.requestPort.getAddrRanges();
            return parent.metadataRequestPort.getAddrRanges();
        }

        bool tryTiming(PacketPtr pkt) override { return true; }

      private:
        AbstractIntegrityVerifier& parent;
    };

    /**
     * Process an incoming metadata request from metadata cache. This implies a
     * cache miss or other cache coherence operation.
     *
     * Most of the time, this will involve simply forwarding the request to
     * memory to handle.
     */
    bool processMetadataReq(PacketPtr pkt);

    // Port that receives responses from the metadata cache and sends requests.
    class MetadataRequestPort : public QueuedRequestPort
    {
      public:
        MetadataRequestPort(const std::string &_name,
          AbstractIntegrityVerifier &_parent);

      protected:
        bool recvTimingResp(PacketPtr pkt) override;
        void recvFunctionalSnoop(PacketPtr pkt) override {
          fatal("Unimplemented");
        }
        Tick recvAtomicSnoop(PacketPtr pkt) override {
          fatal("Unimplemented");
          return 0;
        }
        void recvTimingSnoopReq(PacketPtr pkt) override { };

        void recvRangeChange() override {
            parent.responsePort.sendRangeChange();
        }

        bool isSnooping() const override {
            return false;
            // return parent.responsePort.isSnooping();
        }

      private:
        AbstractIntegrityVerifier& parent;
    };

    /**
     * Process an incoming metadata response from metadata cache. This implies
     * the metadata cache has stored the data, already has been verified, and
     * is ready to use.
     */
    bool processMetadataResp(PacketPtr pkt);

    RequestPort requestPort;
    ResponsePort responsePort;
    MetadataRequestPort metadataRequestPort;
    MetadataResponsePort metadataResponsePort;

    ReqPacketQueue reqQueue;
    RespPacketQueue respQueue;
    SnoopRespPacketQueue snoopRespQueue;
    ReqPacketQueue metadataReqQueue;
    RespPacketQueue metadataRespQueue;
    SnoopRespPacketQueue metadataSnoopRespQueue;

    /**
     * Use the upstream cache as a metadata cache.
     */
    bool unifiedUpstreamCache;

    /**
     * We must enforce that packets leave in the same order that they were
     * received.
     */
    std::queue<RequestPtr> requestQueue;

    /**
     * We enforce metadata requests that arrive (from cache for memory) should
     * leave to memory in the same order that they were received.
     */
    std::queue<RequestPtr> metadataRequestQueue;

    /**
     * We must enforce that packets leave in the same order that they were
     * received.
     */
    std::queue<RequestPtr> responseQueue;

    /**
     * Requests that are ready to send (integrity verification complete or
     * otherwise ready).
     */
    std::unordered_set<RequestPtr> requestReady;

    /**
     * Responses that are ready to send (integrity verification complete or
     * otherwise ready).
     */
    std::unordered_set<RequestPtr> responseReady;

    /**
     * Get the parent integrity node ID associated with a packet.
     */
    size_t getParentNode(PacketPtr pkt);

    bool parentNodeIsSecureRoot(PacketPtr pkt);

    /**
     * Check if the parent node for a given packet is currently pending
     * eviction.
     */
    bool parentNodeIsPendingEviction(PacketPtr pkt);

    /**
     * Get the address of a node within the integrity structure.
     *
     * This determines the mapping between tree nodes and memory arrangement.
     *
     * NOTE: This assumes that the size of one node in the tree is equivalent
     * in size to a cache line.
     */
    Addr getIntegrityNodeLocation(size_t node);

    /**
     * Create a metadata request for the parent node of a given packet `pkt`.
     *
     * Returns the metadata request packet.
     */
    PacketPtr generateMetadataRequest(PacketPtr pkt);

    /**
     * Create a metadata request for a specific metadata node.
     *
     * Returns the metadata request packet.
     */
    PacketPtr generateMetadataRequest(size_t node);


    /**
     * An event that represents when we should re-attempt to process a packet.
     * This may happen if a packet is rejected from memory (i.e., due to the
     * parent being in the middle of eviction). Memory doesn't always have the
     * process to resend a packet.
     */
    class RetryVerifyEvent : public Event
    {
      private:
        // Pointer to the related verifier object.
        AbstractIntegrityVerifier *verifier;

        // Pointer to the original request packet that we are verifying.
        PacketPtr pkt;

      public:
        RetryVerifyEvent(
          AbstractIntegrityVerifier *verifier,
          PacketPtr pkt
        ) : Event(Default_Pri, AutoDelete),
          verifier(verifier),
          pkt(pkt)
        { }

        void process() override {
          verifier->handlePacket(pkt);
        }
    };

    /**
     * Save a packet to retry its processing later.
     */
    void saveRetryVerify(PacketPtr pkt);

    /**
     * An event that represents when we should re-attempt to do the requesting
     * of a packet. This may happen if a packet is rejected due to another
     * packet of the same address being processed.
     *
     * This does not necessarily mean a packet is retrying to be verified. This
     * could be a packet that doesn't necessarily need verification to proceed
     * proceed, but to prevent ordering issues, a packet might need to delay.
     */
    class RetryReqEvent : public Event
    {
      private:
        // Pointer to the related verifier object.
        AbstractIntegrityVerifier *verifier;

        // Pointer to the original request packet.
        PacketPtr pkt;

      public:
        RetryReqEvent(
          AbstractIntegrityVerifier *verifier,
          PacketPtr pkt
        ) : Event(Default_Pri, AutoDelete),
          verifier(verifier),
          pkt(pkt)
        { }

        void process() override {
          verifier->processReq(pkt);
        }
    };

    /**
     * Save a packet to retry its request later.
     */
    void saveRetryReq(PacketPtr pkt);

    /**
     * Handle a packet for triggering integrity verification.
     *
     * For this case, this is for handling integrity verification of read
     * responses or write requests, and verification must complete before
     * they are forwarded to their destination.
     *
     * @return Whether the packet is accepted or not. If the packet is not
     *         accepted, it is the responsibility of the component that sent
     *         the packet to retry.
     */
    bool handlePacket(PacketPtr pkt);

    /**
     * An event that represents when the hash generation for the data in a
     * response packet is finished. This will usually trigger verification
     * by comparing the generated hash to a parent integrity node.
     */
    class HashCompletionEvent : public Event
    {
      private:
        // Pointer to the related verifier object.
        AbstractIntegrityVerifier *verifier;

        // Pointer to the original request packet that we are verifying.
        PacketPtr pkt;

      public:
        HashCompletionEvent(
          AbstractIntegrityVerifier *verifier,
          PacketPtr pkt
        ) : Event(Default_Pri, AutoDelete),
          verifier(verifier),
          pkt(pkt)
        { }

        void process() override {
          verifier->completeIntegrityHash(pkt);
        }
    };

    /**
     * Called when hash generation for a (read) response packet is received.
     */
    void completeIntegrityHash(PacketPtr pkt);

    /**
     * Called when a (parent) node has been verified and can be used to
     * verify a child node.
     */
    void notifyParentReceived(PacketPtr pkt, uint64_t node_completed);

    /**
     * An event that represents when the XOR (data verification) for a packet
     * and its fetched parent data is computed. This will usually trigger
     * sending the packet to its intended destination.
     */
    class XorCompletionEvent : public Event
    {
      private:
        // Pointer to the related verifier object.
        AbstractIntegrityVerifier *verifier;

        // Pointer to the original request packet that we are verifying.
        PacketPtr pkt;

      public:
        XorCompletionEvent(
          AbstractIntegrityVerifier *verifier,
          PacketPtr pkt
        ) : Event(Default_Pri, AutoDelete),
          verifier(verifier),
          pkt(pkt)
        { }

        void process() override {
          verifier->completeXor(pkt);
        }
    };

    /**
     * Attempt to perform an XOR (data verification) for a packet. This may not
     * succeed if not all conditions are met.
     */
    bool attemptXor(PacketPtr pkt);

    /**
     * Called when an XOR (data verification) has finished for a packet and
     * it can continue to completion.
     */
    void completeXor(PacketPtr pkt);

    /**
     * Called when a request has been verified. At this point, a read response
     * can be properly forwarded back to the CPU, a writeback can be properly
     * forwarded to memory, or in the case of a metadata request, the metadata
     * can be inserted into the cache.
     */
    void completeIntegrityVerification(PacketPtr pkt);

    /**
     * Mark a request as received by adding it to the proper tracking
     * structures.
     *
     * This should be called as soon as a request arrives.
     *
     * Associates a request with its packet and notes the arrival of the
     * request, so that the correct order may be maintained.
     */
    void markReqReceived(PacketPtr pkt);

    void markMetadataReqReceived(PacketPtr pkt);

    /**
     * Mark a response as received by ensuring it has arrived and update
     * the proper tracking structures.
     *
     * This should be called as soon as a response arrives.
     */
    void markRespReceived(PacketPtr pkt);

    /**
     * Schedule a request to go to memory.
     *
     * Non-metadata requests will be mandated to be sent in the same order that
     * they were received in.
     *
     * NOTE: For data requests, it is expected that they have already been
     * added to `packetLookup`.
     */
    void schedReq(PacketPtr pkt);

    /**
     * Schedule a response to go to the CPU.
     *
     * Responses will be mandated to be sent in the same order they were
     * received in.
     */
    void schedResp(PacketPtr pkt);

    /**
     * Schedule a metadata request to go to the metadata cache.
     */
    void schedMetadataReq(PacketPtr pkt);

    /**
     * Schedule a metadata response to go to the metadata cache.
     */
    void schedMetadataResp(PacketPtr pkt);

    /**
     * Send a request to memory.
     *
     * The packet will be sent at the next available time.
     *
     * This expects the packet to already be added to `packetLookup`.
     */
    void sendReqToMem(PacketPtr pkt);

    /**
     * Send a response to the CPU.
     *
     * The packet will be sent at the next available time.
     *
     * This expects the packet to already be added to `packetLookup`.
     */
    void sendRespToCpu(PacketPtr pkt);

    /**
     * Send a request to the metadata cache.
     */
    void sendReqToMetadataCache(PacketPtr pkt);

    /**
     * Send a response to the metadata cache.
     *
     * This is used, for example, when verification for some metadata has
     * completed and it can now be stored in the cache.
     */
    void sendRespToMetadataCache(PacketPtr pkt);

    /**
     * Track when each request arrives (when it is accepted).
     */
    std::unordered_map<RequestPtr, Tick> arrivalTime;

    /**
     * Make note of when a (request) packet is about to be scheduled for
     * sending to memory.
     *
     * This is used for finding timing information for how long packets take
     * before they return to this component.
     */
    void markReqStart(PacketPtr pkt);

    /**
     * Make node of when a (response) packet is accepted from memory.
     */
    void markReqEnd(PacketPtr pkt);


    /**
     * Reverse search for a packet from its request pointer.
     */
    std::unordered_map<RequestPtr, PacketPtr> packetLookup;

    /**
     * Add a pairing of a request pointer with a packet pointer. As the request
     * pointer is stored within the packet, only the packet is needed here.
     */
    void addToPacketLookup(PacketPtr pkt);

    /**
     * Update a pairing of a request pointer with a packet pointer.
     */
    void updatePacketLookup(PacketPtr pkt);

    /**
     * Remove a pairing of a request pointer with a packet pointer. As the
     * request pointer is stored within the packet, only the packet is needed
     * here.
     */
    void removeFromPacketLookup(PacketPtr pkt);


    /**
     * Store the outstanding requests for integrity metadata. This associates
     * a tree ID with a pointer to the (child) request(s) that caused this.
     */
    std::unordered_multimap<uint64_t, RequestPtr> outstandingMetadataRequests;

    void addToOutstandingMetadataRequests(uint64_t node, PacketPtr pkt);

    void removeFromOutstandingMetadataRequests(uint64_t node, PacketPtr pkt);

    bool hasOutstandingMetadataRequest(PacketPtr pkt);

    bool hasOutstandingMetadataRequest(uint64_t node, PacketPtr pkt);

    /**
     * Store the outstanding evictions for integrity metadata. This associates
     * a (parent) tree ID with a tree ID to be evicted and the request that
     * will have metadata to cache that takes the place of the evicted entry.
     */
    std::unordered_multimap<
      uint64_t,
      std::pair<uint64_t, RequestPtr>
    > outstandingMetadataEvictions;


    /**
     * Check if an address corresponds to any packets currently pending
     * verification.
     */
    bool addrInOIV(Addr addr);


    /**
     * A sanity checking function that ensures the `packetLookup` list is
     * maintained with recent packets. If there are packets that are in this
     * list for too long, it may indicate that there is a logical error and
     * packets are lost or otherwise not handled correctly.
     */
    void sanityCheckPacketLookup();



  protected:
    /**
     * Delay a request by some number of ticks.
     *
     * @return Ticks to delay packet.
     */
    virtual Tick delayReq(PacketPtr pkt) { return 0; }

    /**
     * Delay a response by some number of ticks.
     *
     * @return Ticks to delay packet.
     */
    virtual Tick delayResp(PacketPtr pkt) { return 0; }

    /**
     * Delay a snoop response by some number of ticks.
     *
     * @return Ticks to delay packet.
     */
    virtual Tick delaySnoopResp(PacketPtr pkt) { return 0; }

  private:
    // Stats

    struct IntegrityVerifierStats : public statistics::Group
    {
      IntegrityVerifierStats(AbstractIntegrityVerifier *parent);

      void preDumpStats() override;

      std::string name() const {
        return parent->name() + ".stats";
      }

      AbstractIntegrityVerifier *parent;

      /**
       * Set of all cache lines accessed.
       *
       * Useful for seeing the memory footprint.
       */
      std::unordered_set<Addr> accessedCacheLines;

      statistics::Scalar memoryFootprint;
      statistics::Scalar dataUsedDram;
      statistics::Scalar dataUsedDramOs;
      statistics::Scalar dataUsedDramIntegrity;
      statistics::Scalar dataUsedCxl;
      statistics::Scalar dataUsedCxlOs;
      statistics::Scalar dataUsedCxlIntegrity;
      statistics::Scalar dataUsedOs;
      statistics::Scalar dataUsedIntegrity;

      statistics::Scalar requestsHandled;
      statistics::Scalar bytesHandled;
      statistics::Scalar metadataReqHandled;
      statistics::Scalar metadataBytesHandled;
      statistics::Scalar dataReqHandled;
      statistics::Scalar dataBytesHandled;

      statistics::Scalar reqHandledDram;
      statistics::Scalar reqHandledDramOs;
      statistics::Scalar reqHandledDramIntegrity;
      statistics::Scalar reqHandledCxl;
      statistics::Scalar reqHandledCxlOs;
      statistics::Scalar reqHandledCxlIntegrity;

      statistics::Scalar reqHandledTransDram;
      statistics::Scalar reqHandledTransDramOs;
      statistics::Scalar reqHandledTransDramIntegrity;
      statistics::Scalar reqHandledTransCxl;
      statistics::Scalar reqHandledTransCxlOs;
      statistics::Scalar reqHandledTransCxlIntegrity;

      statistics::Scalar bytesHandledDram;
      statistics::Scalar bytesHandledDramOs;
      statistics::Scalar bytesHandledDramIntegrity;
      statistics::Scalar bytesHandledCxl;
      statistics::Scalar bytesHandledCxlOs;
      statistics::Scalar bytesHandledCxlIntegrity;

      statistics::Scalar bytesHandledTransDram;
      statistics::Scalar bytesHandledTransDramOs;
      statistics::Scalar bytesHandledTransDramIntegrity;
      statistics::Scalar bytesHandledTransCxl;
      statistics::Scalar bytesHandledTransCxlOs;
      statistics::Scalar bytesHandledTransCxlIntegrity;

      statistics::Scalar totalRequestingTime;
      statistics::Scalar totalMetadataReqTime;
      statistics::Scalar totalDataReqTime;

      statistics::Scalar totalReqTimeDram;
      statistics::Scalar totalReqTimeDramOs;
      statistics::Scalar totalReqTimeDramIntegrity;
      statistics::Scalar totalReqTimeCxl;
      statistics::Scalar totalReqTimeCxlOs;
      statistics::Scalar totalReqTimeCxlIntegrity;

      statistics::Scalar totalReqTimeTransDram;
      statistics::Scalar totalReqTimeTransDramOs;
      statistics::Scalar totalReqTimeTransDramIntegrity;
      statistics::Scalar totalReqTimeTransCxl;
      statistics::Scalar totalReqTimeTransCxlOs;
      statistics::Scalar totalReqTimeTransCxlIntegrity;

      statistics::Formula avgReqLatency;
      statistics::Formula avgMetadataReqLatency;
      statistics::Formula avgDataReqLatency;

      statistics::Formula avgReqTimeDram;
      statistics::Formula avgReqTimeDramOs;
      statistics::Formula avgReqTimeDramIntegrity;
      statistics::Formula avgReqTimeCxl;
      statistics::Formula avgReqTimeCxlOs;
      statistics::Formula avgReqTimeCxlIntegrity;

      statistics::Formula avgReqTimeTransDram;
      statistics::Formula avgReqTimeTransDramOs;
      statistics::Formula avgReqTimeTransDramIntegrity;
      statistics::Formula avgReqTimeTransCxl;
      statistics::Formula avgReqTimeTransCxlOs;
      statistics::Formula avgReqTimeTransCxlIntegrity;
    } stats;


    // DEBUGGING

    /**
     * Show the contents of `arrivalTime`.
     */
    std::string printArrivalTime();

    void fullDebugOutput();

};

/**
 * Delay packets by a constant time. Delays can be specified
 * separately for read requests, read responses, write requests, and
 * write responses.
 *
 * This class does not delay snoops or requests/responses that are
 * neither reads or writes.
 */
class IntegrityVerifier : public AbstractIntegrityVerifier
{
  public:
    PARAMS(IntegrityVerifier);

    IntegrityVerifier(const Params &p);

  protected:
    Tick delayReq(PacketPtr pkt) override;
    Tick delayResp(PacketPtr pkt) override;

  protected: // Params
    const Tick readReqDelay;
    const Tick readRespDelay;

    const Tick writeReqDelay;
    const Tick writeRespDelay;
};

} // namespace gem5

#endif //__MEM_INTEGRITY_VERIFIER_HH__
