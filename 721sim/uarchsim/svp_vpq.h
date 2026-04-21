#ifndef SVP_VPQ_H
#define SVP_VPQ_H

#include <cinttypes>
#include <vector>
#include <cassert>

class SVPVPQ {
public:
   struct SVPEntry {
      bool valid;
      uint64_t tag;
      uint64_t retired_value;
      int64_t stride;
      uint64_t confidence;
      uint64_t instance;

      SVPEntry()
         : valid(false), tag(0), retired_value(0), stride(0), confidence(0), instance(0) {}
   };

   struct VPQEntry {
      bool valid;
      uint64_t pc;
      uint64_t value;
      bool value_ready;

      VPQEntry()
         : valid(false), pc(0), value(0), value_ready(false) {}
   };

   struct TailCheckpoint {
      unsigned int tail;
      bool tail_phase;
      bool valid;

      TailCheckpoint()
         : tail(0), tail_phase(false), valid(false) {}
   };

private:
   std::vector<SVPEntry> svp;
   std::vector<VPQEntry> vpq;
   std::vector<TailCheckpoint> checkpoints;

   unsigned int vpq_head;
   unsigned int vpq_tail;
   bool vpq_head_phase;
   bool vpq_tail_phase;

   unsigned int vpq_size;
   unsigned int num_checkpoints;
   unsigned int index_bits;
   unsigned int tag_bits;
   uint64_t conf_max;

   inline uint64_t index_mask() const {
      if (index_bits == 0) return 0;
      return ((1ULL << index_bits) - 1ULL);
   }

   inline uint64_t tag_mask() const {
      if (tag_bits == 0) return 0;
      return ((1ULL << tag_bits) - 1ULL);
   }

   inline unsigned int svp_num_entries() const {
      return (index_bits == 0) ? 1U : (1U << index_bits);
   }

   unsigned int get_index(uint64_t pc) const;
   uint64_t get_tag(uint64_t pc) const;
   bool hit(uint64_t pc, unsigned int &idx) const;

   bool vpq_empty() const;
   bool vpq_full() const;
   void vpq_advance_head();
   void vpq_advance_tail();

   unsigned int count_inflight_instances(uint64_t pc) const;

public:
   SVPVPQ(unsigned int _vpq_size,
          unsigned int _num_checkpoints,
          unsigned int _index_bits,
          unsigned int _tag_bits,
          uint64_t _conf_max);

   bool can_allocate(unsigned int n) const;

   bool allocate_vpq(uint64_t pc, unsigned int &vpq_index);

   bool predict(uint64_t pc, uint64_t &predicted_value, bool &confident);

   void deposit_value(unsigned int vpq_index, uint64_t value);

   bool head_ready() const;
   void retire_train();

   void checkpoint(unsigned int branch_id);
   void rollback_to_checkpoint(unsigned int branch_id);
   void full_squash();

   uint64_t storage_bits() const;
};

#endif