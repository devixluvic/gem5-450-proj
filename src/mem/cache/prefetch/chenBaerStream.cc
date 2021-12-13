/*
 * Devon Lutz
 * SFU ID #301349016
 * CMPT 450 - Computer Architecture
 * Group 3 - Project - Chen-Baer style streaming prefetcher implementation
 * November 2021
 * Implementing the paper available at 
 * http://web.cecs.pdx.edu/~alaa/ece587/papers/chen_ieeetoc_1995.pdf
 */


#include "mem/cache/prefetch/chenBaerStream.hh"

#include <cassert>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/random.hh"
#include "base/trace.hh"
#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "mem/cache/replacement_policies/base.hh"
#include "params/ChenBaerPrefetcher.hh"

namespace Prefetcher {

ChenBaer::RPTEntry::RPTEntry() {
    // Following the structure of other Gem5 table entries here,
    // call this invalidate
    invalidate();
}

// invalidate: basically, initialize this entry.
void ChenBaer::RPTEntry::invalidate() {
    this->tag = 0;
    this->prev_addr = 0;
    this->stride = 0;
    this->state = INITIAL;
    this->lastHit = 0;
}

ChenBaer::ChenBaer(const ChenBaerPrefetcherParams *p)
  : Queued(p), numPrefetchesToGenerate(p->degree) {}

// Actual function to calculate the prefetch here.
void ChenBaer::calculatePrefetch(const PrefetchInfo &pfi,
                                    std::vector<AddrPriority> &addresses) {


    // Increment internal prefetcher counter:
    this->currentHit++;

    if (!pfi.hasPC()) {
        // We cannot prefetch without a tag to count with!
        DPRINTF(HWPrefetch, "Ignoring request with no PC.\n");
        return;
    }

    // Get required packet info
    Addr requestAddr = pfi.getAddr();
    Addr requestTag = pfi.getPC();

    // Search for this tag in the RPT:
    RPTEntry *preexistingEntry = this->findEntry(requestTag);

    if (preexistingEntry != nullptr) {
        // Paper case A.2: There is a corresponding entry.
        // We have an entry in our RPT! We will use this instead of adding a new one.
        DPRINTF(HWPrefetch, "Entry found in RPT for tag %x\n", preexistingEntry->tag);
        preexistingEntry->lastHit = currentHit; // Keep track of last time entry was used...

        // First, update the entry:
        EntryState currentState = preexistingEntry->state;
        bool correctStridePrediction = (requestAddr == (preexistingEntry->prev_addr + preexistingEntry->stride));

        if ((currentState == INITIAL) && (!correctStridePrediction)) {
            // This is the first case given by the paper.
            // "When incorrect and state is initial"
            // Set prev_addr to addr, stride to addr - prev_addr,
            // and state to TRANSIENT
            preexistingEntry->stride = requestAddr - preexistingEntry->prev_addr;
            preexistingEntry->prev_addr = requestAddr;
            preexistingEntry->state = TRANSIENT;
        }

        if ((currentState != NOPRED) && correctStridePrediction) {
            // This is the second case given by the paper.
            // Moving towards steady state if not already there...
            preexistingEntry->prev_addr = requestAddr;
            preexistingEntry->state = STEADY;
        }

        if ((currentState == STEADY) && !correctStridePrediction) {
            // This is the third case given by the paper
            // Steady state is over, back to initialization
            preexistingEntry->prev_addr = requestAddr;
            preexistingEntry->state = INITIAL;
        }

        if ((currentState == TRANSIENT) && !correctStridePrediction) {
            // This is the fourth case given by the paper
            // Detection of an irregular pattern.
            preexistingEntry->stride = requestAddr - preexistingEntry->prev_addr;
            preexistingEntry->prev_addr = requestAddr;
            preexistingEntry->state = NOPRED;
        }

        if ((currentState == NOPRED) && correctStridePrediction) {
            // This is the fifth case given by the paper
            // Correct prediction made, cautiously return to normal
            preexistingEntry->prev_addr = requestAddr;
            preexistingEntry->state = TRANSIENT;
        }

        if ((currentState == NOPRED) && !correctStridePrediction) {
            // This is the sixth case given by the paper
            // Prediction is still bad. Try updating stride.
            preexistingEntry->stride = requestAddr - preexistingEntry->prev_addr;
            preexistingEntry->prev_addr = requestAddr;
        }
        
        // Now, move on to the prefetch cases.
        // Only enter case B.2 (generate prefetch)
        // if the state we are in is not NOPRED:

        if (preexistingEntry->state != NOPRED) {
            // Generate some number of prefetches:
            for (int d = 1; d <= numPrefetchesToGenerate; d++) {
                // Ensure the prefetch is at least one cache line size:
                // blkSize is a subclass parameter.
                int prefetch_stride = preexistingEntry->stride;
                if (abs(prefetch_stride) < blkSize) {
                    prefetch_stride = (prefetch_stride < 0) ? -blkSize : blkSize;
                }

                Addr addrToPrefetch = requestAddr + d * prefetch_stride;

                // Adding a new AddrPriority to 'addresses' is what generates
                // a prefetch:
                addresses.push_back(AddrPriority(addrToPrefetch, 0));
            }
        }

    } else {
        // Paper case A.1: There is no corresponding entry.
        // Make new entry for this instruction.

        // This function will handle all the logistics of this for us.
        DPRINTF(HWPrefetch, "Adding new entry to table with tag %x and address %x\n", requestTag, requestAddr);
        this->addNewEntry(requestTag, requestAddr);
    }
}

// findEntry: Helper function to search for entries in
// our RPT.
// Returns pointer to a valid one if found, 
// nullptr if no entry with that tag exists.
ChenBaer::RPTEntry* ChenBaer::findEntry(Addr tag) {
    // Foreach-style search is not good for efficiency.
    // But for this proof of concept, it'll do...

    RPTEntry* foundEntry = nullptr;
    for (RPTEntry& currentEntry: this->ReferencePredictionTable) {
        if (currentEntry.tag == tag) {
            foundEntry = &currentEntry;
        }
    }
    return foundEntry;
}

// addNewEntry: Helper function to ensure spacing for 
// RPT entries and adding a new one based on the paper rules.
void ChenBaer::addNewEntry(Addr tag, Addr address) {
    // Ensure we have space:
    if (this->ReferencePredictionTable.size() >= maxNumEntries) {
        removeOldestEntry();
    }
    // Make a new RPTEntry and set initial data 
    // based on the paper in A.1:
    ChenBaer::RPTEntry newEntry;
    newEntry.tag = tag;
    newEntry.prev_addr = address;
    newEntry.state = INITIAL;

    // Add this to the RPT:
    this->ReferencePredictionTable.push_back(newEntry);
}

// removeOldestEntry: Free space in the RPT for a new entry.
void ChenBaer::removeOldestEntry() {
    int oldestIdx = 0;
    int smallestHit = currentHit;
    for (int i = 0; i < this->ReferencePredictionTable.size(); i++) {
        if (this->ReferencePredictionTable[i].lastHit < smallestHit) {
            oldestIdx = i;
            smallestHit = this->ReferencePredictionTable[i].lastHit;
        }
    }
    // Remove this one from the table:
    this->ReferencePredictionTable.erase(this->ReferencePredictionTable.begin() + oldestIdx);
}

} // namespace Prefetcher


Prefetcher::ChenBaer*
ChenBaerPrefetcherParams::create()
{
    return new Prefetcher::ChenBaer(this);
}
