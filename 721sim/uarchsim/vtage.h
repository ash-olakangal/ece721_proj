#ifndef _VTAGE_H_
#define _VTAGE_H_

#include <cstdint>
#include <vector>

class VTAGEPredictor {
public:
   struct Prediction {
      bool available;
      bool confident;
      uint64_t value;
      int provider;   // 0 = base, 1..N = tagged tables, -1 = none
   };

   VTAGEPredictor(unsigned base_entries = 1024,
                  unsigned num_tagged_tables = 4,
                  unsigned tagged_entries = 512,
                  unsigned tag_bits = 10,
                  unsigned conf_bits = 3,
                  unsigned conf_threshold = 2,
                  unsigned path_hist_bits = 64);

   Prediction predict(uint64_t pc) const;

   // Train on every eligible committed result.
   void train(uint64_t pc, uint64_t value);

   // Update the provider that was actually used, if VTAGE supplied it.
   void train_provider(uint64_t pc, uint64_t value, bool correct, int provider);

   // Retired branch history update
   void update_path_history(uint64_t pc, bool taken);

   uint64_t storage_bits() const;
   uint64_t storage_bytes() const { return (storage_bits() + 7) / 8; }

private:
   struct BaseEntry {
      bool valid = false;
      uint64_t value = 0;
      uint8_t conf = 0;
   };

   struct TaggedEntry {
      bool valid = false;
      uint64_t tag = 0;
      uint64_t value = 0;
      uint8_t conf = 0;
      uint8_t useful = 0;
   };

   unsigned base_entries;
   unsigned num_tagged_tables;
   unsigned tagged_entries;
   unsigned tag_bits;
   unsigned conf_bits;
   unsigned conf_threshold;
   unsigned conf_max;
   unsigned path_hist_bits;

   std::vector<BaseEntry> base_table;
   std::vector<std::vector<TaggedEntry>> tagged_tables;
   std::vector<unsigned> history_lengths;
   uint64_t path_history;

   uint64_t mix64(uint64_t x) const;
   unsigned get_base_index(uint64_t pc) const;
   unsigned get_tagged_index(uint64_t pc, unsigned bank) const;
   uint64_t get_tag(uint64_t pc, unsigned bank) const;
};

#endif
