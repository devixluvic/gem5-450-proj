/*
 * Devon Lutz
 * SFU ID #301349016
 * CMPT 450 - Computer Architecture
 * Group 3 - Project - Chen-Baer style streaming prefetcher header
 * November 2021
 */


#ifndef __MEM_CACHE_PREFETCH_CHENBAER_HH__
#define __MEM_CACHE_PREFETCH_CHENBAER_HH__

#include <string>
#include <unordered_map>
#include <vector>
#include <queue>

#include "base/sat_counter.hh"
#include "base/types.hh"
// #include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/queued.hh"
// #include "mem/cache/replacement_policies/replaceable_entry.hh"
// #include "mem/cache/tags/indexing_policies/set_associative.hh"
#include "mem/packet.hh"

class BaseIndexingPolicy;
class BaseReplacementPolicy;
struct ChenBaerPrefetcherParams;

namespace Prefetcher {


class ChenBaer : public Queued
{
  protected:

    /**
     * Reference prediction table type, holding values
     * as described in the paper.
     */
    // struct ReferencePredictionTable {
    //     int numEntries;

    //     ReferencePredictionTable(int size)
    //       : numEntries(size) { };
    // };
    int maxNumEntries = 512;
    uint64_t currentHit = 0;

    // How many prefetches do we want to generate? i.e. base + 4, base + 4 and base + 8, etc.
    // TODO experiment with this - compare misses vs hits...
    int numPrefetchesToGenerate;

    enum EntryState{INITIAL, TRANSIENT, STEADY, NOPRED};

    // An entry for the reference prediction table.
    struct RPTEntry {
        RPTEntry();

        // This function is part of the constructor... probably
        // should not have used it but I was following the Gem5
        // structure from the other prefetchers
        void invalidate();

        // Follow the entry structure from the paper.
        // NOTE: "Addr" type is Gem5 typedef'd uint64_t
        Addr tag; // This is the PC of the instruction we stride for
        Addr prev_addr; // This is the previous address, used for stride calculations
        int stride; // This is used for future address calculations
        EntryState state; // Saturation counter for init, steady, transient etc states using enum.
        uint64_t lastHit;

        bool operator() (RPTEntry first, RPTEntry second) {
          return first.lastHit < second.lastHit;
        }
    };

    // This is our reference prediction table - holding RPTEntries.
    // Code to manage this will likely NOT be optimized.
    // More for proof-of-concept, since any extra runtime from this prefetcher's
    // internal workings will not count towards IPC penalties in the output stats. 
    std::vector<RPTEntry> ReferencePredictionTable;

    // If we do not find an entry for a PC, add it to RPT:
    void addNewEntry(Addr tag, Addr address);

    // Check for an entry with the given PC tag. If no match, 
    // returns nullptr.
    RPTEntry* findEntry(Addr tag);

    // Helper for addNewEntry: removeOldestEntry
    // frees space in the RPT by removing the entry
    // with the smallest lastHit value.
    void removeOldestEntry();

  public:
    // Default constructor 
    ChenBaer(const ChenBaerPrefetcherParams *p);

    // The main prefetcher interface function that actually does the work
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;
};

} // namespace Prefetcher

#endif // __MEM_CACHE_PREFETCH_CHENBAER_HH__
