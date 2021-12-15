/**
 * Copyright (c) 2019 Metempsy Technology Consulting
 * All rights reserved.
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

 /**
  * Implementation of the SpatioMemory Streaming Prefetcher (SMS)
  * Reference:
  *    Spatio memory streaming.
  *    Somogyi, S., Wenisch, T. F., Ailamaki, A., & Falsafi, B. (2009).
  *    ACM SIGARCH Computer Architecture News, 37(3), 69-80.
  *
  * Notes:
  * - The functionality described in the paper as Streamed Value Buffer (SVB)
  *   is not implemented here, as this is handled by the QueuedPrefetcher class
  */

#ifndef __MEM_CACHE_PREFETCH_SPATIO_MEMORY_STREAM_HMM_HH__
#define __MEM_CACHE_PREFETCH_SPATIO_MEMORY_STREAM_HMM_HH__

#include <vector>
#include <queue>

#include "base/circular_queue.hh"
#include "base/sat_counter.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/queued.hh"

struct SMS_HMMPrefetcherParams;

namespace Prefetcher {

class SMS_HMM : public Queued
{
    /** Size of each spatial region */
    const size_t spatialRegionSize;
    /** log_2 of the spatial region size */
    const size_t spatialRegionSizeBits;
    /** Previous Spatial Address */
    Addr previous_spatial_region = 0;

    /**
     * Entry data type for the Active Generation Table (AGT) and the Pattern
     * Sequence Table (PST)
     */
    struct ActiveGenerationTableEntry : public TaggedEntry {
        /** Physical address of the spatial region */
        Addr paddress;
        /** PC that started this generation */
        Addr pc;
        /** Counter to keep track of the interleaving between sequences */
        unsigned int seqCounter;

        /** Sequence entry data type */
        struct SequenceEntry {
            /** 2-bit confidence counter */
            SatCounter counter;
            /** Offset, in cache lines, within the spatial region */
            unsigned int offset;
            /** Intearleaving position on the global access sequence */
            unsigned int delta;
            SequenceEntry() : counter(2), offset(0), delta(0)
            {}
        };
        /** Sequence of accesses */
        std::vector<SequenceEntry> sequence;

        ActiveGenerationTableEntry(int num_positions)
          : TaggedEntry(), paddress(0), pc(0),
            seqCounter(0), sequence(num_positions)
        {
        }

        void
        invalidate() override
        {
            TaggedEntry::invalidate();
            paddress = 0;
            pc = 0;
            seqCounter = 0;
            for (auto &seq_entry : sequence) {
                seq_entry.counter.reset();
                seq_entry.offset = 0;
                seq_entry.delta = 0;
            }
        }

        /**
         * Update the entry data with an entry from a generation that just
         * ended. This operation can not be done with the copy constructor,
         * becasuse the TaggedEntry component must not be copied.
         * @param e entry which generation has ended
         */
        void update(ActiveGenerationTableEntry const &e)
        {
            paddress = e.paddress;
            pc = e.pc;
            seqCounter = e.seqCounter;
            sequence = e.sequence;
        }

        /**
         * Add a new access to the sequence
         * @param offset offset in cachelines within the spatial region
         */
        void addOffset(unsigned int offset) {
            // Search for the offset in the deltas array, if it exist, update
            // the corresponding counter, if not, add the offset to the array
            for (auto &seq_entry : sequence) {
                if (seq_entry.counter > 0) {
                    if (seq_entry.offset == offset) {
                        seq_entry.counter++;
                    }
                } else {
                    // If the counter is 0 it means that this position is not
                    // being used, and we can allocate the new offset here
                    seq_entry.counter++;
                    seq_entry.offset = offset;
                    seq_entry.delta = seqCounter;
                    break;
                }
            }
            seqCounter = 0;
        }
    };

    /** Filter Table (FT) */
    AssociativeSet<ActiveGenerationTableEntry> filterTable;
    /** Active Generation Table (AGT) */
    AssociativeSet<ActiveGenerationTableEntry> activeGenerationTable;
    /** Pattern Sequence Table (PST) */
    AssociativeSet<ActiveGenerationTableEntry> patternSequenceTable;
    /**Markov Table is a vector pair of spatial region and its priority queue
    - priority queue is made up of spatial region and its access counts
    */
    struct MarkovEntry{
        Addr sp_region;
        struct nextSpatialAccess{
            Addr spatial_addr;
            int access_count = 0;

            nextSpatialAccess(Addr sp_add):spatial_addr(sp_add){}
        };
        std::vector<nextSpatialAccess> spatialAccesses;

        MarkovEntry(Addr entry_sp_region):sp_region(entry_sp_region){}

        static bool cmp(nextSpatialAccess &sp1, nextSpatialAccess &sp2){
            return sp1.access_count > sp2.access_count;
        }        

        /** Update the access count if found*/
        void findAccessANDupdate(Addr sp_add){            
            // find the spatial region then update count
            for(int i = 0; i < spatialAccesses.size(); i++){
                if(spatialAccesses[i].spatial_addr == sp_add){
                    spatialAccesses[i].access_count+=1;
                    break;
                }
            }
            // sort the spatial accessess vector to ensure highest access is at the beginning
            std::sort(spatialAccesses.begin(), spatialAccesses.end(), cmp);
        }

        /** return the spatial address with the highest occurrence*/
        Addr predictAddress(){
            std::sort(spatialAccesses.begin(), spatialAccesses.end(), cmp);
            return spatialAccesses[0].spatial_addr;
        }
        
    };
    
    std::vector<MarkovEntry> markovTable;

    /** Update the count of spatial region in Markov Table*/
    void updateMarkovTable(Addr sp_add){
        for(int i = 0; i < markovTable.size(); i++){
            if(markovTable[i].sp_region == previous_spatial_region){
                markovTable[i].findAccessANDupdate(sp_add);
                break;   
            }
        }
    }

    /** return the predicted spatial region access*/
    Addr markoveTablePredictSpatialAddress(){
        Addr address = 0;
        for(int i = 0; i < markovTable.size(); i++){
            if(markovTable[i].sp_region == previous_spatial_region){
                address = markovTable[i].predictAddress();
                break;   
            }
        }
        return address;
    }

    /** nullify victim entry*/
    void markovTablefindVictim(Addr victim){
        for(int i = 0; i < markovTable.size(); i++){
            if(markovTable[i].sp_region == victim){
                markovTable[i].sp_region = 0;
                markovTable[i].spatialAccesses.clear();
                break;   
            }
        }
    }

    void markovTableAddEntry(Addr markovEntry){
        markovTable.emplace_back(markovEntry);
    }

    /** Counter to keep the count of accesses between trigger accesses */
    unsigned int lastTriggerCounter;

    /** Checks if the active generations have ended */
    void checkForActiveGenerationsEnd();

  public:
    SMS_HMM(const SMS_HMMPrefetcherParams* p);
    ~SMS_HMM() = default;

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;
};

} // namespace Prefetcher

#endif//__MEM_CACHE_PREFETCH_SPATIO_MEMORY_STREAMING_HH__
