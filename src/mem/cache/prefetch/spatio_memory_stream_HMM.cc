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

#include "mem/cache/prefetch/spatio_memory_stream_HMM.hh"

#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "params/SMS_HMMPrefetcher.hh"

namespace Prefetcher {

SMS_HMM::SMS_HMM(const SMS_HMMPrefetcherParams *p)
  : Queued(p), spatialRegionSize(p->spatial_region_size),
    spatialRegionSizeBits(floorLog2(p->spatial_region_size)),
    //reconstructionEntries(p->reconstruction_entries),
    filterTable           (p->active_generation_table_assoc,
                          p->active_generation_table_entries,
                          p->active_generation_table_indexing_policy,
                          p->active_generation_table_replacement_policy,
                          ActiveGenerationTableEntry(
                              spatialRegionSize / blkSize)),
    activeGenerationTable(p->active_generation_table_assoc,
                          p->active_generation_table_entries,
                          p->active_generation_table_indexing_policy,
                          p->active_generation_table_replacement_policy,
                          ActiveGenerationTableEntry(
                              spatialRegionSize / blkSize)),
    patternSequenceTable(p->pattern_sequence_table_assoc,
                         p->pattern_sequence_table_entries,
                         p->pattern_sequence_table_indexing_policy,
                         p->pattern_sequence_table_replacement_policy,
                         ActiveGenerationTableEntry(
                             spatialRegionSize / blkSize))
    //rmob(p->region_miss_order_buffer_entries)
{
    fatal_if(!isPowerOf2(spatialRegionSize),
        "The spatial region size must be a power of 2.");
}

void
SMS_HMM::checkForActiveGenerationsEnd()
{
    // This prefetcher operates attached to the L1 and it observes all
    // accesses, this guarantees that no evictions are missed

    // Iterate over all entries, if any recorded cacheline has been evicted,
    // the generation finishes, move the entry to the PST
    for (auto &agt_entry : activeGenerationTable) {
        if (agt_entry.isValid()) {
            bool generation_ended = false;
            bool sr_is_secure = agt_entry.isSecure();
            for (auto &seq_entry : agt_entry.sequence) {
                if (seq_entry.counter > 0) {
                    Addr cache_addr =
                        agt_entry.paddress + seq_entry.offset * blkSize;
                    if (!inCache(cache_addr, sr_is_secure) &&
                            !inMissQueue(cache_addr, sr_is_secure)) {
                        generation_ended = true;
                        break;
                    }
                }
            }
            if (generation_ended) {
                // PST is indexed using the PC (secure bit is unused)
                ActiveGenerationTableEntry *pst_entry =
                    patternSequenceTable.findEntry(agt_entry.pc,
                                                    false /*unused*/ );
                if (pst_entry == nullptr) {
                    // Tipically an entry will not exist
                    pst_entry = patternSequenceTable.findVictim(agt_entry.pc);
                    assert(pst_entry != nullptr);
                    patternSequenceTable.insertEntry(agt_entry.pc,
                            false /*unused*/, pst_entry);
                } else {
                    patternSequenceTable.accessEntry(pst_entry);
                }
                // If the entry existed, this will update the values, if not,
                // this also sets the values of the entry
                pst_entry->update(agt_entry);
                // Free the AGT entry
                activeGenerationTable.invalidate(&agt_entry);
            }
        }
    }
}

void
SMS_HMM::calculatePrefetch(const PrefetchInfo &pfi,
                                   std::vector<AddrPriority> &addresses)
{
    if (!pfi.hasPC()) {
        DPRINTF(HWPrefetch, "Ignoring request with no PC.\n");
        return;
    }
    DPRINTF(HWPrefetch, "Hello World from HMM model\n");

    Addr pc = pfi.getPC();
    bool is_secure = pfi.isSecure();
    // Spatial region address
    Addr sr_addr = pfi.getAddr() / spatialRegionSize;
    Addr paddr = pfi.getPaddr();

    //DPRINTF(HWPrefetch, "PC: %#10X\n", pc);
    //DPRINTF(HWPrefetch, "Spatial Region address: %#10X\n", sr_addr);
    //DPRINTF(HWPrefetch, "Spatial Region physical address: %#10X\n", paddr);

    // Offset in cache-lines within the spatial region
    Addr sr_offset = (pfi.getAddr() % spatialRegionSize) / blkSize;
    //DPRINTF(HWPrefetch, "Spatial Region offset: %#10X\n", sr_offset);

    // Step 4: Fig 2 of Spatial Streaming Paper
    // Check if any active generation has ended
    checkForActiveGenerationsEnd();

    ActiveGenerationTableEntry *agt_entry =
        activeGenerationTable.findEntry(sr_addr, is_secure);
    if (agt_entry != nullptr) {
        // Step3: Fig 2 of Spatial Streaming Paper
        // found an entry in the AGT, entry is currently being recorded,
        // add the offset
        activeGenerationTable.accessEntry(agt_entry);
        agt_entry->addOffset(sr_offset);
        lastTriggerCounter += 1;
        updateMarkovTable(sr_addr);
        //DPRINTF(HWPrefetch, "Updated AGT entry with: %#10x\n", agt_entry->paddress);
    } else {
        // Not found, this is the first access (Trigger access)

        // Consult Markov Table to predict which spatial address will be used next
        Addr sp_addr = predictSpatialAddress();

        // Consult PST to predict which blk will be access
        ActiveGenerationTableEntry *pst_entry = patternSequenceTable.findEntry(sr_addr, is_secure);

        if(pst_entry != nullptr){
            // PST has a record
            // move predicted pattern into cache
            //DPRINTF(HWPrefetch, "SMS returns to cache: %#10X\n", pst_entry->paddress);

            //TODO: concatenate markov prediction with spatial pattern
            addresses.push_back(AddrPriority(pst_entry->paddress,0));
        } else {
            // Step 1: Fig 2 of Spatial Streaming Paper - search filter table
            ActiveGenerationTableEntry *ft_entry = filterTable.findEntry(sr_addr, is_secure);

            if(ft_entry != nullptr){
                // Step 2: Fig 2 of Spatial Streaming Paper
                //found an entry in filter table (spatial region is currently being access)

                // allocate a new AGT entry
                ActiveGenerationTableEntry *new_agt_entry = activeGenerationTable.findVictim(sr_addr);
                assert(agt_entry != nullptr);
                activeGenerationTable.insertEntry(sr_addr, is_secure, agt_entry);
                new_agt_entry->pc = pc;
                new_agt_entry->paddress = paddr;
                new_agt_entry->addOffset(sr_offset);
                //DPRINTF(HWPrefetch, "Created AGT entry with: %#10x\n", new_agt_entry->paddress);

                // Update Markov Table
                updateMarkovTable(sr_addr);
                
            } else {
                // find victim in Filter Table and Markov Table
                ft_entry = filterTable.findVictim(sr_addr);
                assert(ft_entry != nullptr);

                markovTablefindVictim(sr_addr);
                
                // alloocate a new FT entry
                filterTable.insertEntry(sr_addr, is_secure, ft_entry);
                ft_entry->pc = pc;
                ft_entry->addOffset(sr_offset);

                markovTableAddEntry(sr_addr);

                //DPRINTF(HWPrefetch, "INSERTENTRY(): spatio_region: %#10X, pc: %#10x, physical addr: %#10x\n", sr_addr, pc, paddr);
                //DPRINTF(HWPrefetch, "Created FT entry with: %#10x\n", ft_entry->pc);
                ActiveGenerationTableEntry *new_ft_entry = filterTable.findEntry(sr_addr, is_secure);
                if(new_ft_entry != nullptr){
                   //DPRINTF(HWPrefetch, "CONGRATS: SUCCESS in inserting new FT entry found!!\n");
                } else {
                    //DPRINTF(HWPrefetch, "FAILED: insertion of new FT failed\n");
                }
            }
        }              
       
    }
    // increase the seq Counter for other entries
    //TODO: figure out why this for-loop is necessary
    for (auto &agt_e : activeGenerationTable) {
        if (agt_e.isValid() && agt_entry != &agt_e) {
            agt_e.seqCounter += 1;
        }
    }

    previous_spatial_region = sr_addr;
}

// void
// SMS_HMM::reconstructSequence(
//     CircularQueue<RegionMissOrderBufferEntry>::iterator rmob_it,
//     std::vector<AddrPriority> &addresses)
// {
//     std::vector<Addr> reconstruction(reconstructionEntries, MaxAddr);
//     unsigned int idx = 0;

//     // Process rmob entries from rmob_it (most recent with address = sr_addr)
//     // to the latest one
//     for (auto it = rmob_it; it != rmob.end() && (idx < reconstructionEntries);
//         it++) {
//         reconstruction[idx] = it->srAddress * spatialRegionSize;
//         idx += (it+1)->delta + 1;
//     }

//     // Now query the PST with the PC of each RMOB entry
//     idx = 0;
//     for (auto it = rmob_it; it != rmob.end() && (idx < reconstructionEntries);
//         it++) {
//         ActiveGenerationTableEntry *pst_entry =
//             patternSequenceTable.findEntry(it->pstAddress, false /* unused */);
//         if (pst_entry != nullptr) {
//             patternSequenceTable.accessEntry(pst_entry);
//             for (auto &seq_entry : pst_entry->sequence) {
//                 if (seq_entry.counter > 1) {
//                     // 2-bit counter: high enough confidence with a
//                     // value greater than 1
//                     Addr rec_addr = it->srAddress * spatialRegionSize +
//                         seq_entry.offset;
//                     unsigned ridx = idx + seq_entry.delta;
//                     // Try to use the corresponding position, if it has been
//                     // already used, look the surrounding positions
//                     if (ridx < reconstructionEntries &&
//                         reconstruction[ridx] == MaxAddr) {
//                         reconstruction[ridx] = rec_addr;
//                     } else if ((ridx + 1) < reconstructionEntries &&
//                         reconstruction[ridx + 1] == MaxAddr) {
//                         reconstruction[ridx + 1] = rec_addr;
//                     } else if ((ridx + 2) < reconstructionEntries &&
//                         reconstruction[ridx + 2] == MaxAddr) {
//                         reconstruction[ridx + 2] = rec_addr;
//                     } else if ((ridx > 0) &&
//                         ((ridx - 1) < reconstructionEntries) &&
//                         reconstruction[ridx - 1] == MaxAddr) {
//                         reconstruction[ridx - 1] = rec_addr;
//                     } else if ((ridx > 1) &&
//                         ((ridx - 2) < reconstructionEntries) &&
//                         reconstruction[ridx - 2] == MaxAddr) {
//                         reconstruction[ridx - 2] = rec_addr;
//                     }
//                 }
//             }
//         }
//         idx += (it+1)->delta + 1;
//     }

//     for (Addr pf_addr : reconstruction) {
//         if (pf_addr != MaxAddr) {
//             addresses.push_back(AddrPriority(pf_addr, 0));
//         }
//     }
// }

} // namespace Prefetcher

Prefetcher::SMS_HMM*
SMS_HMMPrefetcherParams::create()
{
   return new Prefetcher::SMS_HMM(this);
}
