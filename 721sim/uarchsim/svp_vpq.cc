#include "svp_vpq.h"

SVPVPQ::SVPVPQ(unsigned int _vpq_size,
               unsigned int _num_checkpoints,
               unsigned int _index_bits,
               unsigned int _tag_bits,
               uint64_t _conf_max)
   : vpq_head(0),
     vpq_tail(0),
     vpq_head_phase(false),
     vpq_tail_phase(false),
     vpq_size(_vpq_size),
     num_checkpoints(_num_checkpoints),
     index_bits(_index_bits),
     tag_bits(_tag_bits),
     conf_max(_conf_max) {

   assert(vpq_size > 0);
   assert(num_checkpoints > 0);

   svp.resize(svp_num_entries());
   vpq.resize(vpq_size);
   checkpoints.resize(num_checkpoints);
}

unsigned int SVPVPQ::get_index(uint64_t pc) const {
   uint64_t shifted = (pc >> 2); // ignore low 2 alignment bits
   if (index_bits == 0) return 0;
   return (unsigned int)(shifted & index_mask());
}

uint64_t SVPVPQ::get_tag(uint64_t pc) const {
   uint64_t shifted = (pc >> 2);
   if (tag_bits == 0) return 0;
   return ((shifted >> index_bits) & tag_mask());
}

bool SVPVPQ::hit(uint64_t pc, unsigned int &idx) const {
   idx = get_index(pc);
   const SVPEntry &e = svp[idx];
   if (!e.valid) return false;
   if (tag_bits == 0) return true;
   return (e.tag == get_tag(pc));
}

bool SVPVPQ::vpq_empty() const {
   return (vpq_head == vpq_tail) && (vpq_head_phase == vpq_tail_phase);
}

bool SVPVPQ::vpq_full() const {
   return (vpq_head == vpq_tail) && (vpq_head_phase != vpq_tail_phase);
}

void SVPVPQ::vpq_advance_head() {
   vpq_head++;
   if (vpq_head == vpq_size) {
      vpq_head = 0;
      vpq_head_phase = !vpq_head_phase;
   }
}

void SVPVPQ::vpq_advance_tail() {
   vpq_tail++;
   if (vpq_tail == vpq_size) {
      vpq_tail = 0;
      vpq_tail_phase = !vpq_tail_phase;
   }
}

bool SVPVPQ::can_allocate(unsigned int n) const {
   if (n == 0) return true;

   unsigned int used;
   if (vpq_full()) {
      used = vpq_size;
   } else if (vpq_empty()) {
      used = 0;
   } else if (vpq_tail_phase == vpq_head_phase) {
      used = vpq_tail - vpq_head;
   } else {
      used = vpq_size - vpq_head + vpq_tail;
   }

   unsigned int free_entries = vpq_size - used;
   return (free_entries >= n);
}

bool SVPVPQ::allocate_vpq(uint64_t pc, unsigned int &vpq_index) {
   if (vpq_full()) return false;

   vpq_index = vpq_tail;
   vpq[vpq_tail].valid = true;
   vpq[vpq_tail].pc = pc;
   vpq[vpq_tail].value = 0;
   vpq[vpq_tail].value_ready = false;

   vpq_advance_tail();
   return true;
}

bool SVPVPQ::predict(uint64_t pc, uint64_t &predicted_value, bool &confident) {
   unsigned int idx;
   if (!hit(pc, idx)) {
      confident = false;
      return false;
   }

   SVPEntry &e = svp[idx];

   // Speculatively bump instance first, then predict.
   e.instance++;

   int64_t pred = (int64_t)e.retired_value + ((int64_t)e.instance * e.stride);
   predicted_value = (uint64_t)pred;
   confident = (e.confidence >= conf_max);

   return true;
}

void SVPVPQ::deposit_value(unsigned int vpq_index, uint64_t value) {
   assert(vpq_index < vpq_size);
   assert(vpq[vpq_index].valid);

   vpq[vpq_index].value = value;
   vpq[vpq_index].value_ready = true;
}

bool SVPVPQ::head_ready() const {
   if (vpq_empty()) return false;
   return (vpq[vpq_head].valid && vpq[vpq_head].value_ready);
}

unsigned int SVPVPQ::count_inflight_instances(uint64_t pc) const {
   unsigned int count = 0;
   unsigned int i = vpq_head;
   bool phase = vpq_head_phase;

   while (!((i == vpq_tail) && (phase == vpq_tail_phase))) {
      if (vpq[i].valid && vpq[i].pc == pc) {
         count++;
      }

      i++;
      if (i == vpq_size) {
         i = 0;
         phase = !phase;
      }
   }

   return count;
}

void SVPVPQ::retire_train() {
   assert(!vpq_empty());
   assert(vpq[vpq_head].valid);
   assert(vpq[vpq_head].value_ready);

   uint64_t pc = vpq[vpq_head].pc;
   uint64_t value = vpq[vpq_head].value;

   // Pop head first, per lecture flow.
   vpq[vpq_head].valid = false;
   vpq_advance_head();

   unsigned int idx;
   if (hit(pc, idx)) {
      SVPEntry &e = svp[idx];

      int64_t new_stride = (int64_t)value - (int64_t)e.retired_value;

      if (new_stride == e.stride) {
         if (e.confidence < conf_max) {
            e.confidence++;
         }
      } else {
         e.confidence = 0;
         e.stride = new_stride;
      }

      e.retired_value = value;

      if (e.instance > 0) {
         e.instance--;
      }
   } else {
      idx = get_index(pc);
      SVPEntry &e = svp[idx];

      e.valid = true;
      e.tag = get_tag(pc);
      e.confidence = 0;
      e.retired_value = value;
      e.stride = (int64_t)value; // matches professor's example initialization
      e.instance = count_inflight_instances(pc);
   }
}

void SVPVPQ::checkpoint(unsigned int branch_id) {
   assert(branch_id < num_checkpoints);
   checkpoints[branch_id].tail = vpq_tail;
   checkpoints[branch_id].tail_phase = vpq_tail_phase;
   checkpoints[branch_id].valid = true;
}

void SVPVPQ::rollback_to_checkpoint(unsigned int branch_id) {
   assert(branch_id < num_checkpoints);
   if (!checkpoints[branch_id].valid) return;

   unsigned int rollback_tail = checkpoints[branch_id].tail;
   bool rollback_tail_phase = checkpoints[branch_id].tail_phase;

   // Walk entries that will be squashed and decrement speculative instance counters.
   unsigned int i = rollback_tail;
   bool phase = rollback_tail_phase;

   while (!((i == vpq_tail) && (phase == vpq_tail_phase))) {
      if (vpq[i].valid) {
         unsigned int idx;
         if (hit(vpq[i].pc, idx)) {
            if (svp[idx].instance > 0) {
               svp[idx].instance--;
            }
         }

         vpq[i].valid = false;
         vpq[i].value_ready = false;
      }

      i++;
      if (i == vpq_size) {
         i = 0;
         phase = !phase;
      }
   }

   vpq_tail = rollback_tail;
   vpq_tail_phase = rollback_tail_phase;
}

void SVPVPQ::full_squash() {
   // Clear all in-flight VPQ state.
   for (unsigned int i = 0; i < vpq_size; i++) {
      vpq[i].valid = false;
      vpq[i].value_ready = false;
   }

   vpq_head = 0;
   vpq_tail = 0;
   vpq_head_phase = false;
   vpq_tail_phase = false;

   // Reset speculative instance counters only.
   for (unsigned int i = 0; i < svp.size(); i++) {
      svp[i].instance = 0;
   }

   for (unsigned int i = 0; i < num_checkpoints; i++) {
      checkpoints[i].valid = false;
   }
}

uint64_t SVPVPQ::storage_bits() const {
   uint64_t svp_bits = 0;
   uint64_t tag_storage = tag_bits;
   uint64_t conf_bits = 64; // over-approximation, fine for now
   uint64_t value_bits = 64;
   uint64_t stride_bits = 64;
   uint64_t instance_bits = 64;
   uint64_t valid_bits = 1;

   svp_bits = (uint64_t)svp.size() * (valid_bits + tag_storage + conf_bits + value_bits + stride_bits + instance_bits);

   uint64_t vpq_entry_bits = 1 + 64 + 64 + 1; // valid + pc + value + value_ready
   uint64_t vpq_bits = (uint64_t)vpq.size() * vpq_entry_bits;

   return (svp_bits + vpq_bits);
}