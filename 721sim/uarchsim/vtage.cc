#include "vtage.h"
#include <cassert>

VTAGEPredictor::VTAGEPredictor(unsigned base_entries_,
                               unsigned num_tagged_tables_,
                               unsigned tagged_entries_,
                               unsigned tag_bits_,
                               unsigned conf_bits_,
                               unsigned conf_threshold_,
                               unsigned path_hist_bits_)
   : base_entries(base_entries_),
     num_tagged_tables(num_tagged_tables_),
     tagged_entries(tagged_entries_),
     tag_bits(tag_bits_),
     conf_bits(conf_bits_),
     conf_threshold(conf_threshold_),
     conf_max((1u << conf_bits_) - 1u),
     path_hist_bits(path_hist_bits_),
     base_table(base_entries_),
     tagged_tables(num_tagged_tables_, std::vector<TaggedEntry>(tagged_entries_)),
     history_lengths(num_tagged_tables_, 0),
     path_history(0)
{
   assert(base_entries > 0);
   assert(num_tagged_tables > 0);
   assert(tagged_entries > 0);

   // Geometric-ish history lengths
   unsigned h = 8;
   for (unsigned i = 0; i < num_tagged_tables; i++) {
      history_lengths[i] = h;
      h *= 2;
   }
}

uint64_t VTAGEPredictor::mix64(uint64_t x) const {
   x ^= (x >> 33);
   x *= 0xff51afd7ed558ccdULL;
   x ^= (x >> 33);
   x *= 0xc4ceb9fe1a85ec53ULL;
   x ^= (x >> 33);
   return x;
}

unsigned VTAGEPredictor::get_base_index(uint64_t pc) const {
   return (unsigned)(mix64(pc >> 2) % base_entries);
}

unsigned VTAGEPredictor::get_tagged_index(uint64_t pc, unsigned bank) const {
   uint64_t hmask;
   if (history_lengths[bank] >= 64)
      hmask = path_history;
   else
      hmask = path_history & ((1ULL << history_lengths[bank]) - 1ULL);

   uint64_t key = (pc >> 2) ^ mix64(hmask ^ (0x9e3779b97f4a7c15ULL + bank));
   return (unsigned)(key % tagged_entries);
}

uint64_t VTAGEPredictor::get_tag(uint64_t pc, unsigned bank) const {
   uint64_t hmask;
   if (history_lengths[bank] >= 64)
      hmask = path_history;
   else
      hmask = path_history & ((1ULL << history_lengths[bank]) - 1ULL);

   uint64_t key = mix64((pc >> 2) ^ (hmask << 1) ^ bank);
   if (tag_bits >= 64) return key;
   return key & ((1ULL << tag_bits) - 1ULL);
}

VTAGEPredictor::Prediction VTAGEPredictor::predict(uint64_t pc) const {
   Prediction p{};
   p.available = false;
   p.confident = false;
   p.value = 0;
   p.provider = -1;

   // Longest-history matching tagged table wins
   for (int bank = (int)num_tagged_tables - 1; bank >= 0; bank--) {
      unsigned idx = get_tagged_index(pc, (unsigned)bank);
      const TaggedEntry &e = tagged_tables[(unsigned)bank][idx];
      if (e.valid && e.tag == get_tag(pc, (unsigned)bank)) {
         p.available = true;
         p.confident = (e.conf >= conf_threshold);
         p.value = e.value;
         p.provider = bank + 1;
         return p;
      }
   }

   // Fallback base predictor
   const BaseEntry &b = base_table[get_base_index(pc)];
   if (b.valid) {
      p.available = true;
      p.confident = false;   // conservative hybrid: base predictor is never used confidently
      p.value = b.value;
      p.provider = 0;
   }

   return p;
}

void VTAGEPredictor::train(uint64_t pc, uint64_t value) {
   // Always train base
   BaseEntry &b = base_table[get_base_index(pc)];
   if (!b.valid) {
      b.valid = true;
      b.value = value;
      b.conf = 0;
   } else {
      if (b.value == value) {
         if (b.conf < conf_max) b.conf++;
      } else {
         if (b.conf > 0) b.conf--;
         b.value = value;
      }
   }

   // Also allocate/update one tagged table on committed stream.
   // Use longest-history table first; replace first invalid else low-conf entry.
   for (int bank = (int)num_tagged_tables - 1; bank >= 0; bank--) {
      unsigned idx = get_tagged_index(pc, (unsigned)bank);
      TaggedEntry &e = tagged_tables[(unsigned)bank][idx];

      if (!e.valid || e.tag == get_tag(pc, (unsigned)bank)) {
         if (!e.valid) {
            e.valid = true;
            e.tag = get_tag(pc, (unsigned)bank);
            e.value = value;
            e.conf = 0;
            e.useful = 0;
         } else {
            if (e.value == value) {
               if (e.conf < conf_max) e.conf++;
            } else {
               if (e.conf > 0) e.conf--;
               e.value = value;
            }
         }
         return;
      }
   }
}

void VTAGEPredictor::train_provider(uint64_t pc, uint64_t value, bool correct, int provider) {
   if (provider < 0)
      return;

   if (provider == 0) {
      BaseEntry &b = base_table[get_base_index(pc)];
      if (!b.valid) {
         b.valid = true;
         b.value = value;
         b.conf = 0;
         return;
      }

      b.value = value;
      if (correct) {
         if (b.conf < conf_max) b.conf++;
      } else {
         if (b.conf > 0) b.conf--;
      }
      return;
   }

   unsigned bank = (unsigned)(provider - 1);
   unsigned idx = get_tagged_index(pc, bank);
   TaggedEntry &e = tagged_tables[bank][idx];

   if (!e.valid || e.tag != get_tag(pc, bank)) {
      e.valid = true;
      e.tag = get_tag(pc, bank);
      e.value = value;
      e.conf = 0;
      e.useful = 0;
      return;
   }

   e.value = value;
   if (correct) {
      if (e.conf < conf_max) e.conf++;
      if (e.useful < 3) e.useful++;
   } else {
      if (e.conf > 0) e.conf--;
      e.useful = 0;
   }
}

void VTAGEPredictor::update_path_history(uint64_t pc, bool taken) {
   uint64_t bit = ((pc >> 2) ^ (taken ? 1ULL : 0ULL)) & 1ULL;
   path_history = (path_history << 1) | bit;
   if (path_hist_bits < 64)
      path_history &= ((1ULL << path_hist_bits) - 1ULL);
}

uint64_t VTAGEPredictor::storage_bits() const {
   uint64_t bits = 0;

   // base entry: valid + value + conf
   bits += (uint64_t)base_entries * (1ULL + 64ULL + conf_bits);

   // tagged entry: valid + tag + value + conf + useful
   bits += (uint64_t)num_tagged_tables * (uint64_t)tagged_entries *
           (1ULL + (uint64_t)tag_bits + 64ULL + conf_bits + 2ULL);

   bits += path_hist_bits;

   return bits;
}
