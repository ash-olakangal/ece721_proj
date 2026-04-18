#include "renamer.h"

renamer::renamer(uint64_t n_log_regs, uint64_t n_phys_regs, uint64_t n_branches, uint64_t n_active) 
	    : NUM_LOGICAL_REG(n_log_regs), NUM_PHYSICAL_REG(n_phys_regs), 
	      NUM_BRANCH_CP(n_branches), AL_SIZE(n_active), 
	      RMT(n_log_regs, 0), AMT(n_log_regs, 0), 
	      FreeList(n_phys_regs - n_log_regs),
	      ActiveList(n_active), 
	      PRF(n_phys_regs, 0), PRF_ready(n_phys_regs, true), 
	      Branch_Checkpoint(n_branches, BC_entry(n_log_regs)), 
	      GBM(0) 
	{
	    // Sanity checks
	    assert(NUM_PHYSICAL_REG > NUM_LOGICAL_REG);
	    assert(NUM_BRANCH_CP >= 1 && NUM_BRANCH_CP <= 64);
	    assert(AL_SIZE > 0);

	    ////std::cout << "PRF_readY " << std::endl;
	    for(auto entry : PRF_ready){
	    	////std::cout << entry << std::endl;
	    }
	
	    for(uint64_t i = 0; i < NUM_LOGICAL_REG; i++){
	        AMT[i] = i;
	        RMT[i] = i;    
	    }
	
	    // Correctly initialize FreeList using push()
	    for(uint64_t i = NUM_LOGICAL_REG; i < NUM_PHYSICAL_REG; i++){
	        bool success = FreeList.push(i);
	        assert(success);
	    }
	};

bool renamer::stall_reg(uint64_t bundle_dst){
	uint64_t available;
	if (FreeList.is_full()) available = FreeList.capacity;
	else if (FreeList.is_empty()) available = 0;
	else if (FreeList.tail_phase == FreeList.head_phase) available = FreeList.tail - FreeList.head;
	else available = FreeList.capacity - FreeList.head + FreeList.tail;
	
	return (available < bundle_dst);
}

bool renamer::stall_branch(uint64_t bundle_branch){
	
	//std::cout << "stall_branch" << std::endl;
	uint64_t free_checkpoint=0;

	//std::cout << " before free_checkpoint: " << free_checkpoint << " bundle_branch: " << bundle_branch << "GBM: " << GBM << std::endl; 
	for(int i=0; i<Branch_Checkpoint.size(); i++){
	  if(Branch_Checkpoint[i].free == true) free_checkpoint++;
	}

	//std::cout << " after free_checkpoint: " << free_checkpoint << " bundle_branch: " << bundle_branch << std::endl; 

	return (free_checkpoint < bundle_branch);
}

uint64_t renamer::get_branch_mask(){

	////std::cout << "get_branch_mask" << std::endl;
	return GBM;
}

uint64_t renamer::rename_rsrc(uint64_t log_reg){
	// The sources are renamed from the RMT
	// Indexing into the RMT and returning latest mapping
	
	////std::cout << "rename_rsrc: " << log_reg << std::endl;
	return RMT[log_reg];
}

uint64_t renamer::rename_rdst(uint64_t log_reg){
	// The destination reg are named from head of Free list
	// CHeck if Free list is empty or not?
	
	////std::cout << "rename_rdst" << std::endl;

	assert(!FreeList.is_empty());
	uint64_t free_reg_at_head = FreeList.front(); // get the reg at head of FL
	FreeList.pop_discard(); // increment the head pointer in the free list
	RMT[log_reg] = free_reg_at_head; // update in RMT the new mapping
	return free_reg_at_head;
}


uint64_t renamer::checkpoint(){
	
	//ASSERT free BIT is available
	assert(GBM != 0xFFFFFFFFFFFFFFFFULL);

	uint64_t branch_ID = 0;
	while (branch_ID < NUM_BRANCH_CP && (GBM & (1ULL << branch_ID))) {
        branch_ID++;
    	}

	Branch_Checkpoint[branch_ID].FL_head_index = FreeList.head;
	Branch_Checkpoint[branch_ID].head_phase = FreeList.head_phase;
	Branch_Checkpoint[branch_ID].tail_phase = FreeList.tail_phase;
	Branch_Checkpoint[branch_ID].CP_GBM = GBM;
	Branch_Checkpoint[branch_ID].free = false;
	
	for(int j=0; j<RMT.size(); j++){
	Branch_Checkpoint[branch_ID].CP_RMT[j] = RMT[j];
	}

	GBM = GBM | (1 << branch_ID); 

	//std::cout << "Checkpoint" << std::endl;
	//std::cout << "GBM: " << GBM << " branch_ID: " << branch_ID << std::endl;

	return branch_ID;
}

bool renamer::stall_dispatch(uint64_t bundle_inst){


	////std::cout << "stall_dispatch" << std::endl;

	uint64_t used;
	if (ActiveList.is_full()) used = ActiveList.capacity;
	else if (ActiveList.is_empty()) used = 0;
	else if (ActiveList.tail > ActiveList.head) used = ActiveList.tail - ActiveList.head;
	else used = ActiveList.capacity - ActiveList.head + ActiveList.tail;
	
	return (ActiveList.capacity - used < bundle_inst);
}


uint64_t renamer::dispatch_inst(bool dest_valid,
                       uint64_t log_reg,
                       uint64_t phys_reg,
                       bool load,
                       bool store,
                       bool branch,
                       bool amo,
                       bool csr,
                       uint64_t PC){


	////std::cout << "dispatch_inst" << std::endl;

	assert(!stall_dispatch(1)); // check if there is space for adding a new instr in active list

	AL_entry entry; // temp entry that will be pushed to AL

	entry.dest_flag     = dest_valid;
    	entry.logical_dest  = log_reg;
    	entry.physical_dest = phys_reg;
    	entry.load_flag     = load;
    	entry.store_flag    = store;
    	entry.branch_flag   = branch;
    	entry.amo_flag      = amo;
    	entry.csr_flag      = csr;
    	entry.instr_pc      = PC;
	
	ActiveList.push(entry);
	uint64_t index = (ActiveList.tail == 0) ? (ActiveList.capacity - 1) : (ActiveList.tail - 1); // accounting for case when tail could be 0
	return index;
}

bool renamer::is_ready(uint64_t phys_reg){

	////std::cout << "is_ready" << std::endl;

	return PRF_ready[phys_reg];
}

void renamer::clear_ready(uint64_t phys_reg){

	////std::cout << "clear_ready" << std::endl;

	PRF_ready[phys_reg] = false;
}

uint64_t renamer::read(uint64_t phys_reg){

	////std::cout << "read" << std::endl;

	return PRF[phys_reg];
}

void renamer::set_ready(uint64_t phys_reg){

	////std::cout << "set_ready" << std::endl;

	PRF_ready[phys_reg] = true;
}

void renamer::write(uint64_t phys_reg, uint64_t value){

	////std::cout << "write reg: " << phys_reg << " value: " << value  << std::endl;


	PRF[phys_reg] = value;
}

void renamer::set_complete(uint64_t AL_index){

	////std::cout << "set_complete" << std::endl;

	ActiveList.buffer[AL_index].completed = true;	
}

void renamer::resolve(uint64_t AL_index, uint64_t branch_ID, bool correct){

	//std::cout << "resolve" << std::endl;

	if(correct == false){

		// we need to clear all the branch checkpoints that have the mispred branch id set in their CP_GBM
		uint64_t squashed_mask = GBM ^ Branch_Checkpoint[branch_ID].CP_GBM;

		for (uint64_t i = 0; i < NUM_BRANCH_CP; i++) {
			if (squashed_mask & (1ULL << i)) {
				Branch_Checkpoint[i].free = true;
			}
		}
	
		GBM = Branch_Checkpoint[branch_ID].CP_GBM;

		for(int i=0; i<RMT.size(); i++){
			RMT[i] = Branch_Checkpoint[branch_ID].CP_RMT[i];
		}

		FreeList.head = Branch_Checkpoint[branch_ID].FL_head_index;
		FreeList.head_phase = Branch_Checkpoint[branch_ID].head_phase;

		//ActiveList.tail = AL_index+1;

		//if(ActiveList.tail < ActiveList.head) ActiveList.tail_phase = !ActiveList.head_phase;
		//else ActiveList.tail_phase = ActiveList.head_phase;

		// Squash Active List: Tail points to entry AFTER the branch
		//ActiveList.tail = (AL_index + 1);
		//if (ActiveList.tail == AL_SIZE) { // Check for wrap-around
		//	ActiveList.tail = 0;
		//	ActiveList.tail_phase = !ActiveList.buffer[AL_index].phase; // Use phase of the branch itself
		//} else {
		//	ActiveList.tail_phase = ActiveList.buffer[AL_index].phase;
		//}
		
		uint64_t new_tail = (AL_index + 1) % AL_SIZE;
		
		bool branch_phase = (AL_index >= ActiveList.head) ? ActiveList.head_phase : !ActiveList.head_phase;
		
		if (AL_index == AL_SIZE - 1) {
		    // If the branch was at the very last physical slot, the next slot (tail) 
		    // wraps to 0 and its phase flips.
		    ActiveList.tail = 0;
		    ActiveList.tail_phase = !branch_phase;
		} else {
		    // Otherwise, the tail is just the next slot on the same lap.
		    ActiveList.tail = AL_index + 1;
		    ActiveList.tail_phase = branch_phase;
		}
		
		Branch_Checkpoint[branch_ID].free = true;
	
	}
	else{
		GBM = GBM & ~(1ULL<<branch_ID);
		for(int i=0; i<Branch_Checkpoint.size(); i++){
			Branch_Checkpoint[i].CP_GBM = Branch_Checkpoint[i].CP_GBM & ~(1ULL << branch_ID);
		}
		Branch_Checkpoint[branch_ID].free = true; // instr resolved and entry in checkpoint is now free
	}
}

bool renamer::precommit(bool &completed,
               bool &exception, bool &load_viol, bool &br_misp, bool &val_misp,
               bool &load, bool &store, bool &branch, bool &amo, bool &csr,
	       uint64_t &PC){

	//////std::cout << "precommit" << << std::endl;

	if(ActiveList.is_empty() == false){
		completed = ActiveList.front().completed;
		exception = ActiveList.front().exception;
		load_viol = ActiveList.front().load_violation;
		br_misp = ActiveList.front().branch_mis;
		val_misp = ActiveList.front().value_mis;
		load = ActiveList.front().load_flag;
		store = ActiveList.front().store_flag;
		branch =  ActiveList.front().branch_flag;
		amo = ActiveList.front().amo_flag;
		csr = ActiveList.front().csr_flag;
		PC = ActiveList.front().instr_pc;
	}

	////std::cout << "precommit: PC " << PC << " branch: " << branch << " completed: " << completed << std::endl;

	return(!ActiveList.is_empty());
}

void renamer::commit(){


	////std::cout << "commit" << std::endl;
	//uint64_t head = ActiveList.head;

	assert(ActiveList.is_empty() == false);
	assert(ActiveList.front().completed == true);
	assert(ActiveList.front().exception == false);
	assert(ActiveList.front().load_violation == false);

	if(ActiveList.front().dest_flag){
		// copy the mapping from AMT to FL
		uint64_t logical_reg = ActiveList.front().logical_dest;
		FreeList.push(AMT[logical_reg]);

		// update the AMT to current mapping in AL that is being commited
		AMT[logical_reg] = ActiveList.front().physical_dest;
	}

	//increment the head of AL
	ActiveList.pop_discard();
}

void renamer::squash(){
 // Steps to squash on mispred or exception
 // 1. copy AMT into RMT
 // 2. roll back FL
 // 3. clear AL
 //
 	
	//std::cout << "squash GBM: " << GBM << std::endl;
	
 
	// copying AMT to RMT
	for(int i=0; i<AMT.size(); i++){
		RMT[i] = AMT[i];
	}

	// restoring FL
	FreeList.head = FreeList.tail;
	FreeList.head_phase  = !FreeList.tail_phase;

	// clearing AL
	ActiveList.tail = ActiveList.head;
	ActiveList.tail_phase = ActiveList.head_phase;

	// resetting PRF_ready bits
	for(int i = 0; i < PRF_ready.size(); i++){
		PRF_ready[i] = true;
	}

	GBM = 0; 
	for(int i = 0; i < Branch_Checkpoint.size(); i++){
		Branch_Checkpoint[i].free = true;
	}


}


void renamer::set_exception(uint64_t AL_index){


	////std::cout << "set_exception" << std::endl;

	ActiveList.buffer[AL_index].exception = true;
}

void renamer::set_load_violation(uint64_t AL_index){

	////std::cout << "set_load_violation" << std::endl;

	ActiveList.buffer[AL_index].load_violation = true;
}

void renamer::set_branch_misprediction(uint64_t AL_index){

	////std::cout << "set_branch_misprediction" << std::endl;

	ActiveList.buffer[AL_index].branch_mis = true;
}

void renamer::set_value_misprediction(uint64_t AL_index){

	////std::cout << "set_value_misprediction" << std::endl;

	ActiveList.buffer[AL_index].value_mis = true;
}

bool renamer::get_exception(uint64_t AL_index){

	////std::cout << "get_exception" << std::endl;

	return ActiveList.buffer[AL_index].exception;
}
