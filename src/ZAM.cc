// See the file "COPYING" in the main distribution directory for copyright.

#include "ZAM.h"
#include "CompHash.h"
#include "RE.h"
#include "Frame.h"
#include "Reduce.h"
#include "Scope.h"
#include "ProfileFunc.h"
#include "ScriptAnaly.h"
#include "Trigger.h"
#include "Desc.h"
#include "Reporter.h"
#include "Traverse.h"

// Needed for managing the corresponding values.
#include "File.h"
#include "Func.h"
#include "OpaqueVal.h"

// Just needed for BiFs.
#include "Net.h"
#include "logging/Manager.h"
#include "broker/Manager.h"

static BroType* log_ID_enum_type;


// Count of how often each top of ZOP executed, and how much CPU it
// cumulatively took.
int ZOP_count[OP_NOP+1];
double ZOP_CPU[OP_NOP+1];

// Per-interpreted-expression.
std::unordered_map<const Expr*, double> expr_CPU;


// Tracks per function its maximum remapped interpreter frame size.  We
// can't do this when compiling individual functions since for event handlers
// and hooks it needs to be computed across all of their bodies.
std::unordered_map<const Func*, int> remapped_intrp_frame_sizes;

void finalize_functions(const std::vector<FuncInfo*>& funcs)
	{
	// Given we've now compiled all of the function bodies, we
	// can reset the interpreter frame sizes of each function
	// to be the maximum needed to accommodate all of its
	// remapped bodies.

	if ( remapped_intrp_frame_sizes.size() == 0 )
		// We didn't do remapping.
		return;

	for ( auto& f : funcs )
		{
		auto func = f->func;

		if ( remapped_intrp_frame_sizes.count(func) == 0 )
			// We didn't compile this function, presumably
			// because it contained something like a "when"
			// statement.
			continue;

		// Note, because functions with multiple bodies appear
		// in "funcs" multiple times, but the following doesn't
		// hurt to do more than once.
		func->SetFrameSize(remapped_intrp_frame_sizes[func]);
		}
	}


void report_ZOP_profile()
	{
	for ( int i = 1; i <= OP_NOP; ++i )
		if ( ZOP_count[i] > 0 )
			printf("%s\t%d\t%.06f\n", ZOP_name(ZOp(i)),
				ZOP_count[i], ZOP_CPU[i]);

	for ( auto& e : expr_CPU )
		printf("expr CPU %.06f %s\n", e.second, obj_desc(e.first));
	}


void ZAM_run_time_error(const Stmt* stmt, const char* msg)
	{
	if ( stmt->Tag() == STMT_EXPR )
		{
		auto e = stmt->AsExprStmt()->StmtExpr();
		reporter->ExprRuntimeError(e, "%s", msg);
		}
	else
		fprintf(stderr, "%s: %s\n", msg, obj_desc(stmt));

	ZAM_error = true;
	}

void ZAM_run_time_error(const char* msg, const BroObj* o)
	{
	fprintf(stderr, "%s: %s\n", msg, obj_desc(o));
	ZAM_error = true;
	}


class OpaqueVals {
public:
	OpaqueVals(ZInstAux* _aux)	{ aux = _aux; }

	ZInstAux* aux;
};


// The dynamic state of a global.  Used to construct an array indexed in
// parallel with the globals[] array, which tracks the associated static
// information.
typedef enum {
	GS_UNLOADED,	// global hasn't been loaded
	GS_CLEAN,	// global has been loaded but not modified
	GS_DIRTY,	// loaded-and-modified
} GlobalState;


// Helper functions, to translate NameExpr*'s to slots.  Some aren't
// needed, but we provide a complete set mirroring those for ZInst
// for consistency.
ZInst GenInst(ZAM* m, ZOp op)
	{
	return ZInst(op);
	}

ZInst GenInst(ZAM* m, ZOp op, const NameExpr* v1)
	{
	return ZInst(op, m->Frame1Slot(v1, op));
	}

ZInst GenInst(ZAM* m, ZOp op, const NameExpr* v1, int i)
	{
	auto z = ZInst(op, m->Frame1Slot(v1, op), i);
	z.op_type = OP_VV_I2;
	return z;
	}

ZInst GenInst(ZAM* m, ZOp op, const ConstExpr* c, const NameExpr* v1, int i)
	{
	auto z = ZInst(op, m->Frame1Slot(v1, op), i, c);
	z.op_type = OP_VVC_I2;
	return z;
	}

ZInst GenInst(ZAM* m, ZOp op, const NameExpr* v1, const Expr* e)
	{
	return ZInst(op, m->Frame1Slot(v1, op), e);
	}

ZInst GenInst(ZAM* m, ZOp op, const NameExpr* v1, const NameExpr* v2)
	{
	int nv2 = m->FrameSlot(v2);
	return ZInst(op, m->Frame1Slot(v1, op), nv2);
	}

ZInst GenInst(ZAM* m, ZOp op, const NameExpr* v1, const NameExpr* v2,
		const NameExpr* v3)
	{
	int nv2 = m->FrameSlot(v2);
	int nv3 = m->FrameSlot(v3);
	return ZInst(op, m->Frame1Slot(v1, op), nv2, nv3);
	}

ZInst GenInst(ZAM* m, ZOp op, const NameExpr* v1, const NameExpr* v2,
		const NameExpr* v3, const NameExpr* v4)
	{
	int nv2 = m->FrameSlot(v2);
	int nv3 = m->FrameSlot(v3);
	int nv4 = m->FrameSlot(v4);
	return ZInst(op, m->Frame1Slot(v1, op), nv2, nv3, nv4);
	}

ZInst GenInst(ZAM* m, ZOp op, const ConstExpr* ce)
	{
	return ZInst(op, ce);
	}

ZInst GenInst(ZAM* m, ZOp op, const NameExpr* v1, const ConstExpr* ce)
	{
	return ZInst(op, m->Frame1Slot(v1, op), ce);
	}

ZInst GenInst(ZAM* m, ZOp op, const NameExpr* v1, const ConstExpr* ce,
		const NameExpr* v2)
	{
	int nv2 = m->FrameSlot(v2);
	return ZInst(op, m->Frame1Slot(v1, op), nv2, ce);
	}
ZInst GenInst(ZAM* m, ZOp op, const NameExpr* v1, const NameExpr* v2,
		const ConstExpr* ce)
	{
	int nv2 = m->FrameSlot(v2);
	return ZInst(op, m->Frame1Slot(v1, op), nv2, ce);
	}

ZInst GenInst(ZAM* m, ZOp op, const NameExpr* v1, const NameExpr* v2,
		const NameExpr* v3, const ConstExpr* ce)
	{
	int nv2 = m->FrameSlot(v2);
	int nv3 = m->FrameSlot(v3);
	return ZInst(op, m->Frame1Slot(v1, op), nv2, nv3, ce);
	}
ZInst GenInst(ZAM* m, ZOp op, const NameExpr* v1, const NameExpr* v2,
		const ConstExpr* ce, const NameExpr* v3)
	{
	// Note that here we reverse the order of the arguments; saves
	// us from needing to implement a redundant constructor.
	int nv2 = m->FrameSlot(v2);
	int nv3 = m->FrameSlot(v3);
	return ZInst(op, m->Frame1Slot(v1, op), nv2, nv3, ce);
	}

ZInst GenInst(ZAM* m, ZOp op, const NameExpr* v1, const ConstExpr* c, int i)
	{
	auto z = ZInst(op, m->Frame1Slot(v1, op), i, c);
	z.op_type = OP_VVC_I2;
	return z;
	}
ZInst GenInst(ZAM* m, ZOp op, const NameExpr* v1, const NameExpr* v2, int i)
	{
	int nv2 = m->FrameSlot(v2);
	auto z = ZInst(op, m->Frame1Slot(v1, op), nv2, i);
	z.op_type = OP_VVV_I3;
	return z;
	}
ZInst GenInst(ZAM* m, ZOp op, const NameExpr* v1, const NameExpr* v2,
		int i1, int i2)
	{
	int nv2 = m->FrameSlot(v2);
	auto z = ZInst(op, m->Frame1Slot(v1, op), nv2, i1, i2);
	z.op_type = OP_VVVV_I3_I4;
	return z;
	}
ZInst GenInst(ZAM* m, ZOp op, const NameExpr* v, const ConstExpr* c,
		int i1, int i2)
	{
	auto z = ZInst(op, m->Frame1Slot(v, op), i1, i2, c);
	z.op_type = OP_VVVC_I2_I3;
	return z;
	}

ZInst GenInst(ZAM* m, ZOp op, const NameExpr* v1, const NameExpr* v2,
		const NameExpr* v3, int i)
	{
	int nv2 = m->FrameSlot(v2);
	int nv3 = m->FrameSlot(v3);
	auto z = ZInst(op, m->Frame1Slot(v1, op), nv2, nv3, i);
	z.op_type = OP_VVVV_I4;
	return z;
	}
ZInst GenInst(ZAM* m, ZOp op, const NameExpr* v1, const NameExpr* v2,
		const ConstExpr* c, int i)
	{
	int nv2 = m->FrameSlot(v2);
	auto z = ZInst(op, m->Frame1Slot(v1, op), nv2, i, c);
	z.op_type = OP_VVVC_I3;
	return z;
	}
ZInst GenInst(ZAM* m, ZOp op, const NameExpr* v1, const ConstExpr* c,
		const NameExpr* v2, int i)
	{
	int nv2 = m->FrameSlot(v2);
	auto z = ZInst(op, m->Frame1Slot(v1, op), nv2, i, c);
	z.op_type = OP_VVVC_I3;
	return z;
	}


// Maps first operands and then type tags to operands.
static std::unordered_map<ZOp, std::unordered_map<TypeTag, ZOp>>
	assignment_flavor;

// Maps flavorful assignments to their non-assignment counterpart.
// Used for optimization when we determine that the assigned-to
// value is superfluous.
static std::unordered_map<ZOp, ZOp> assignmentless_op;

// Maps flavorful assignments to what op-type their non-assignment
// counterpart uses.
static std::unordered_map<ZOp, ZAMOpType> assignmentless_op_type;

ZOp AssignmentFlavor(ZOp orig, TypeTag tag)
	{
	static bool did_init = false;

	if ( ! did_init )
		{
		std::unordered_map<TypeTag, ZOp> empty_map;

#include "ZAM-AssignFlavorsDefs.h"

		did_init = true;
		}

	// Map type tag to equivalent, as needed.
	switch ( tag ) {
	case TYPE_BOOL:
	case TYPE_ENUM:
		tag = TYPE_INT;
		break;

	case TYPE_COUNTER:
	case TYPE_PORT:
		tag = TYPE_COUNT;
		break;

	case TYPE_TIME:
	case TYPE_INTERVAL:
		tag = TYPE_DOUBLE;
		break;

	default:
		break;
	}

	ASSERT(assignment_flavor.count(orig) > 0);

	auto orig_map = assignment_flavor[orig];
	ASSERT(orig_map.count(tag) > 0);

	return orig_map[tag];
	}


ZAM::ZAM(BroFunc* f, Scope* _scope, Stmt* _body,
		UseDefs* _ud, Reducer* _rd, ProfileFunc* _pf)
	{
	tag = STMT_COMPILED;
	func = f;
	scope = _scope;
	body = _body;
	body->Ref();
	ud = _ud;
	reducer = _rd;
	pf = _pf;
	frame_size = 0;

	Init();
	}

ZAM::~ZAM()
	{
	if ( fixed_frame )
		{
		// Free slots with explicit memory management.
		for ( auto i = 0; i < managed_slots.size(); ++i )
			{
			auto& v = fixed_frame[managed_slots[i]];
			DeleteManagedType(v, nullptr);
			}

		delete[] fixed_frame;
		}

	Unref(body);
	delete inst_count;
	delete CPU_time;
	delete ud;
	delete reducer;
	delete pf;
	}

Stmt* ZAM::CompileBody()
	{
	curr_stmt = nullptr;

	if ( func->Flavor() == FUNC_FLAVOR_HOOK )
		PushBreaks();

	(void) body->Compile(this);

	if ( LastStmt()->Tag() != STMT_RETURN )
		SyncGlobals(nullptr);

	if ( breaks.size() > 0 )
		{
		ASSERT(breaks.size() == 1);

		if ( func->Flavor() == FUNC_FLAVOR_HOOK )
			{
			// Rewrite the breaks.
			for ( auto b : breaks[0] )
				{
				auto& i = insts1[b.stmt_num];
				delete i;
				i = new ZInst(OP_HOOK_BREAK_X);
				}
			}

		else
			reporter->Error("\"break\" used without an enclosing \"for\" or \"switch\"");
		}

	if ( nexts.size() > 0 )
		reporter->Error("\"next\" used without an enclosing \"for\"");

	if ( fallthroughs.size() > 0 )
		reporter->Error("\"fallthrough\" used without an enclosing \"switch\"");

	if ( catches.size() > 0 )
		reporter->InternalError("untargeted inline return");

	// Make sure we have a (pseudo-)instruction at the end so we
	// can use it as a branch label.
	if ( ! pending_inst )
		pending_inst = new ZInst();

	// Concretize instruction numbers in inst1 so we can
	// easily move through the code.
	for ( auto i = 0; i < insts1.size(); ++i )
		insts1[i]->inst_num = i;

	// Compute which instructions are inside loops.
	for ( auto i = 0; i < insts1.size(); ++i )
		{
		auto inst = insts1[i];

		auto t = inst->target;
		if ( ! t || t == pending_inst )
			continue;

		if ( t->inst_num < i )
			{
			auto j = t->inst_num;

			if ( ! t->loop_start )
				{
				// Loop is newly discovered.
				t->loop_start = true;
				}
			else
				{
				// We're extending an existing loop.  Find
				// its current end.
				auto depth = t->loop_depth;
				while ( j < i &&
					insts1[j]->loop_depth == depth )
					++j;

				ASSERT(insts1[j]->loop_depth == depth - 1);
				}

			// Run from j's current position to i, bumping
			// the loop depth.
			while ( j <= i )
				{
				++insts1[j]->loop_depth;
				++j;
				}
			}

		ASSERT(! inst->target2 || inst->target2->inst_num > i);
		}

	if ( ! analysis_options.no_ZAM_opt )
		OptimizeInsts();

	// Move branches to dead code forward to their successor live code.
	for ( auto i = 0; i < insts1.size(); ++i )
		{
		auto inst = insts1[i];
		if ( ! inst->live )
			continue;

		auto t = inst->target;

		if ( ! t )
			continue;

		inst->target = FindLiveTarget(t);

		if ( inst->target2 )
			inst->target2 = FindLiveTarget(inst->target2);
		}

	// Construct the final program with the dead code eliminated
	// and branches resolved.

	// Make sure we don't include the empty pending-instruction,
	// if any.
	if ( pending_inst )
		pending_inst->live = false;

	// Maps inst1 instructions to where they are in inst2.
	// Dead instructions map to -1.
	std::vector<int> inst1_to_inst2;

	for ( auto i = 0; i < insts1.size(); ++i )
		{
		if ( insts1[i]->live )
			{
			inst1_to_inst2.push_back(insts2.size());
			insts2.push_back(insts1[i]);
			}
		else
			inst1_to_inst2.push_back(-1);
		}

	// Re-concretize instruction numbers, and concretize GoTo's.
	for ( auto i = 0; i < insts2.size(); ++i )
		insts2[i]->inst_num = i;

	for ( auto i = 0; i < insts2.size(); ++i )
		{
		auto inst = insts2[i];

		if ( inst->target )
			{
			RetargetBranch(inst, inst->target, inst->target_slot);

			if ( inst->target2 )
				RetargetBranch(inst, inst->target2,
						inst->target2_slot);
			}
		}

	// Update remapped frame denizens, if any.
	for ( auto i = 0; i < shared_frame_denizens.size(); ++i )
		{
		auto& info = shared_frame_denizens[i];

		for ( auto& start : info.id_start )
			start = inst1_to_inst2[start];

		shared_frame_denizens_final.push_back(info);
		}

	delete pending_inst;

	// Could erase insts1 here to recover memory, but it's handy
	// for debugging.

	if ( analysis_options.report_profile )
		{
		inst_count = new vector<int>;
		inst_CPU = new vector<double>;
		for ( auto i : insts2 )
			{
			inst_count->push_back(0);
			inst_CPU->push_back(0.0);
			}

		CPU_time = new double;
		*CPU_time = 0.0;
		}
	else
		inst_count = nullptr;

	if ( non_recursive )
		{
		fixed_frame = new ZAMValUnion[frame_size];

		for ( auto i = 0; i < managed_slots.size(); ++i )
			fixed_frame[managed_slots[i]].managed_val = nullptr;

		func->UseStaticFrame();
		}

	return this;
	}

void ZAM::Init()
	{
	auto uds = ud->HasUsage(body) ? ud->GetUsage(body) : nullptr;
	auto args = scope->OrderedVars();
	auto nparam = func->FType()->Args()->NumFields();

	num_globals = pf->globals.size();

	for ( auto g : pf->globals )
		{
		GlobalInfo info;
		info.id = g;
		info.slot = AddToFrame(g);
		global_id_to_info[g] = globals.size();
		globals.push_back(info);
		}

	::Ref(scope);
	push_existing_scope(scope);

	for ( auto a : args )
		{
		if ( --nparam < 0 )
			break;

		auto arg_id = a.get();
		if ( uds && uds->HasID(arg_id) )
			LoadParam(arg_id);
		else
			{
			// printf("param %s unused\n", obj_desc(arg_id.get()));
			}
		}

	pop_scope();

	// Assign slots for locals (which includes temporaries).
	for ( auto l : pf->locals )
		{
		// ### should check for unused variables.
		// Don't add locals that were already added because they're
		// parameters.
		if ( ! HasFrameSlot(l) )
			(void) AddToFrame(l);
		}

	// Complain about unused aggregates ... but not if we're inlining,
	// as that can lead to optimizations where they wind up being unused
	// but the original logic for using them was sound.
	if ( ! analysis_options.inliner )
		for ( auto a : pf->inits )
			{
			if ( pf->locals.find(a) == pf->locals.end() )
				reporter->Warning("%s unused", a->Name());
			}

	for ( auto& slot : frame_layout1 )
		{
		// Look for locals with values of types for which
		// we do explicit memory management on (re)assignment.
		auto t = slot.first->Type();
		if ( IsManagedType(t) )
			{
			managed_slots.push_back(slot.second);
			managed_slot_types.push_back(t);
			}
		}

	non_recursive = non_recursive_funcs.count(func) > 0;
	}

void ZAM::OptimizeInsts()
	{
	// Do accounting for targeted statements.
	for ( auto& i : insts1 )
		{
		if ( i->target && i->target->live )
			++(i->target->num_labels);
		if ( i->target2 && i->target2->live )
			++(i->target2->num_labels);
		}

#define TALLY_SWITCH_TARGETS(switches) \
	for ( auto& targs : switches ) \
		for ( auto& targ : targs ) \
			++(targ.second->num_labels);

	TALLY_SWITCH_TARGETS(int_cases);
	TALLY_SWITCH_TARGETS(uint_cases);
	TALLY_SWITCH_TARGETS(double_cases);
	TALLY_SWITCH_TARGETS(str_cases);

	bool something_changed;

	do
		{
		something_changed = false;

		while ( RemoveDeadCode() )
			something_changed = true;

		while ( CollapseGoTos() )
			something_changed = true;

		ComputeFrameLifetimes();

		if ( PruneUnused() )
			something_changed = true;
		}
	while ( something_changed );

	ReMapFrame();
	ReMapInterpreterFrame();
	}

bool ZAM::RemoveDeadCode()
	{
	bool did_removal = false;

	for ( int i = 0; i < int(insts1.size()) - 1; ++i )
		{
		auto i0 = insts1[i];
		auto i1 = insts1[i+1];

		if ( i0->live && i1->live && i0->DoesNotContinue() &&
		     i0->target != i1 && i1->num_labels == 0 )
			{
			did_removal = true;
			KillInst(i1);
			}
		}

	return did_removal;
	}

bool ZAM::CollapseGoTos()
	{
	bool did_collapse = false;

	for ( int i = 0; i < insts1.size(); ++i )
		{
		auto i0 = insts1[i];

		if ( ! i0->live )
			continue;

		auto t = i0->target;
		if ( ! t )
			continue;

		// Note, we don't bother optimizing target2 if present,
		// as those are very rare.

		if ( t->IsUnconditionalBranch() )
			{ // Collapse branch-to-branch.
			did_collapse = true;
			do
				{
				ASSERT(t->live);

				--t->num_labels;
				t = t->target;
				i0->target = t;
				++t->num_labels;
				}
			while ( t->IsUnconditionalBranch() );
			}

		// Collapse branch-to-next-statement, taking into
		// account dead code.
		int j = i + 1;

		bool branches_into_dead = false;
		while ( j < insts1.size() && ! insts1[j]->live )
			{
			if ( t == insts1[j] )
				branches_into_dead = true;
			++j;
			}

		// j now points to the first live instruction after i.
		if ( branches_into_dead ||
		     (j < insts1.size() && t == insts1[j]) ||
		     (j == insts1.size() && t == pending_inst) )
			{ // i0 is branch-to-next-statement
			if ( t != pending_inst )
				--t->num_labels;

			if ( i0->IsUnconditionalBranch() )
				// no point in keeping the branch
				i0->live = false;

			else if ( j < insts1.size() )
				{
				// Update i0 to target the live instruction.
				i0->target = insts1[j];
				++i0->target->num_labels;
				}
			}
		}

	return did_collapse;
	}

bool ZAM::PruneUnused()
	{
	bool did_prune = false;

	for ( int i = 0; i < insts1.size(); ++i )
		{
		auto inst = insts1[i];

		if ( ! inst->live )
			continue;

		if ( inst->IsFrameStore() && ! VarIsAssigned(inst->v1) )
			{
			did_prune = true;
			KillInst(inst);
			}

		if ( inst->IsLoad() && ! VarIsUsed(inst->v1) )
			{
			did_prune = true;
			KillInst(inst);
			}

		if ( ! inst->AssignsToSlot1() )
			continue;

		int slot = inst->v1;
		if ( denizen_ending.count(slot) > 0 ||
		     frame_denizens[slot]->IsGlobal() )
			continue;

		// Assignment to a local that isn't otherwise used.
		if ( ! inst->HasSideEffects() )
			{
			did_prune = true;
			// We don't use this assignment.
			KillInst(inst);
			continue;
			}

		// Transform the instruction into its flavor that doesn't
		// make an assignment.
		switch ( inst->op ) {
		case OP_LOG_WRITE_VVV:
			inst->op = OP_LOG_WRITE_VV;
			inst->op_type = OP_VV;
			inst->v1 = inst->v2;
			inst->v2 = inst->v3;
			break;

		case OP_LOG_WRITE_VVC:
			inst->op = OP_LOG_WRITE_VC;
			inst->op_type = OP_Vc;
			inst->v1 = inst->v2;
			break;

		case OP_BROKER_FLUSH_LOGS_V:
			inst->op = OP_BROKER_FLUSH_LOGS_X;
			inst->op_type = OP_X;
			break;

		default:
			if ( assignmentless_op.count(inst->op) > 0 )
				{
				inst->op_type = assignmentless_op_type[inst->op];
				inst->op = assignmentless_op[inst->op];

				inst->v1 = inst->v2;
				inst->v2 = inst->v3;
				inst->v3 = inst->v4;
				}
			else
				reporter->InternalError("inconsistency in re-flavoring instruction with side effects");
		}

		// While we didn't prune the instruction,
		// we did prune the assignment, so we'll
		// want to reassess variable lifetimes.
		did_prune = true;
		}

	return did_prune;
	}

void ZAM::ComputeFrameLifetimes()
	{
	// Start analysis from scratch, since we can do this repeatedly.
	inst_beginnings.clear();
	inst_endings.clear();

	denizen_beginning.clear();
	denizen_ending.clear();

	for ( auto i = 0; i < insts1.size(); ++i )
		{
		auto inst = insts1[i];
		if ( ! inst->live )
			continue;

		if ( inst->AssignsToSlot1() )
			CheckSlotAssignment(inst->v1, inst);

		// Some special-casing.
		switch ( inst->op ) {
		case OP_NEXT_TABLE_ITER_VV:
		case OP_NEXT_TABLE_ITER_VAL_VAR_VVV:
			{
			// These assign to an arbitrary long list of variables.
			auto iter_vars = inst->c.iter_info;
			auto depth = inst->loop_depth;

			for ( auto v : iter_vars->loop_vars )
				{
				CheckSlotAssignment(v, inst);

				// Also mark it as usage throughout the
				// loop.  Otherwise, we risk pruning the
				// variable if it happens to not be used
				// (which will mess up the iteration logic)
				// or doubling it up with some other value
				// inside the loop (which will fail when
				// the loop var has memory management
				// associated with it).
				ExtendLifetime(v, EndOfLoop(inst, depth));
				}

			// No need to check the additional "var" associated
			// with OP_NEXT_TABLE_ITER_VAL_VAR_VVV as that's
			// a slot-1 assignment.  However, similar to other
			// loop variables, mark this as a usasge.
			if ( inst->op == OP_NEXT_TABLE_ITER_VAL_VAR_VVV )
				ExtendLifetime(inst->v1, EndOfLoop(inst, depth));
			}
			break;

		case OP_SYNC_GLOBALS_X:
			{
			// Extend the lifetime of any modified globals.
			for ( auto g : modified_globals )
				{
				int gs = frame_layout1[g];
				if ( denizen_beginning.count(gs) == 0 )
					// Global hasn't been loaded yet.
					continue;

				ExtendLifetime(gs, EndOfLoop(inst, 1));
				}
			}
			break;

		case OP_INIT_TABLE_LOOP_VVC:
		case OP_INIT_VECTOR_LOOP_VV:
		case OP_INIT_STRING_LOOP_VV:
			{
			// For all of these, the scope of the aggregate
			// being looped over is the entire loop, even
			// if it doesn't directly appear in it, and not
			// just the initializer.  For all three, the
			// aggregate is in v2.
			ASSERT(i < insts1.size() - 1);
			auto succ = insts1[i+1];
			ASSERT(succ->live);
			auto depth = succ->loop_depth;
			ExtendLifetime(inst->v2, EndOfLoop(succ, depth));

			// Important: we skip the usual UsesSlots analysis
			// below since we've already set it, and don't want
			// to perturb ExtendLifetime's consistency check.
			continue;
			}

		default:
			// Look for slots in auxiliary information.
			if ( ! inst->aux )
				break;

			auto aux = inst->aux;
			for ( auto j = 0; j < aux->n; ++j )
				{
				if ( aux->slots[j] < 0 )
					continue;

				ExtendLifetime(aux->slots[j],
						EndOfLoop(inst, 1));
				}
			break;
		}

		int s1, s2, s3, s4;

		if ( ! inst->UsesSlots(s1, s2, s3, s4) )
			continue;

		CheckSlotUse(s1, inst);
		CheckSlotUse(s2, inst);
		CheckSlotUse(s3, inst);
		CheckSlotUse(s4, inst);
		}
	}

void ZAM::ReMapFrame()
	{
	// General approach: go sequentially through the instructions,
	// see which variables begin their lifetime at each, and at
	// that point remap the variables to a suitable frame slot.

	frame1_to_frame2.resize(frame_layout1.size(), -1);
	managed_slots.clear();

	for ( auto i = 0; i < insts1.size(); ++i )
		{
		auto inst = insts1[i];

		if ( inst_beginnings.count(inst) == 0 )
			continue;

		auto vars = inst_beginnings[inst];
		for ( auto v : vars )
			{
			// Don't remap variables whose values aren't actually
			// used.
			int slot = frame_layout1[v];
			if ( denizen_ending.count(slot) > 0 )
				ReMapVar(v, slot, i);
			}
		}

#if 0
	printf("%s frame remapping:\n", func->Name());

	for ( auto i = 0; i < shared_frame_denizens.size(); ++i )
		{
		auto& s = shared_frame_denizens[i];
		printf("*%d (%s) %lu [%d->%d]:",
			i, s.is_managed ? "M" : "N",
			s.ids.size(), s.id_start[0], s.scope_end);

		for ( auto j = 0; j < s.ids.size(); ++j )
			printf(" %s (%d)", s.ids[j]->Name(), s.id_start[j]);

		printf("\n");
		}
#endif

	// Update the globals we track, where we prune globals that
	// didn't wind up being used.  (This can happen because they're
	// only used in interpreted expressions.)
	std::vector<GlobalInfo> used_globals;
	std::vector<int> remapped_globals;

	for ( auto i = 0; i < globals.size(); ++i )
		{
		auto& g = globals[i];
		g.slot = frame1_to_frame2[g.slot];
		if ( g.slot >= 0 )
			{
			remapped_globals.push_back(used_globals.size());
			used_globals.push_back(g);
			}
		else
			remapped_globals.push_back(-1);
		}

	globals = used_globals;
	num_globals = globals.size();

	// Gulp - now rewrite every instruction to update its slot usage.
	// In the process, if an instruction becomes a direct assignment
	// of <slot-n> = <slot-n>, then we remove it.

	int n1_slots = frame1_to_frame2.size();

	for ( auto i = 0; i < insts1.size(); ++i )
		{
		auto inst = insts1[i];

		if ( ! inst->live )
			continue;

		if ( inst->AssignsToSlot1() )
			{
			auto v1 = inst->v1;
			ASSERT(v1 >= 0 && v1 < n1_slots);
			inst->v1 = frame1_to_frame2[v1];
			}

		// Handle special cases.
		switch ( inst->op ) {
		case OP_NEXT_TABLE_ITER_VV:
		case OP_NEXT_TABLE_ITER_VAL_VAR_VVV:
			{
			// Rewrite iteration variables.
			auto iter_vars = inst->c.iter_info;
			for ( auto& v : iter_vars->loop_vars )
				{
				ASSERT(v >= 0 && v < n1_slots);
				v = frame1_to_frame2[v];
				}
			}
			break;

		case OP_DIRTY_GLOBAL_V:
			{
			// Slot v1 of this is an index into globals[]
			// rather than a frame.
			int g = inst->v1;
			ASSERT(remapped_globals[g] >= 0);
			inst->v1 = remapped_globals[g];

			// We *don't* want to UpdateSlots below as
			// that's based on interpreting v1 as slots
			// rather than an index into globals
			continue;
			}

		default:
			// Update slots in auxiliary information.
			if ( ! inst->aux )
				break;

			auto aux = inst->aux;
			for ( auto j = 0; j < aux->n; ++j )
				{
				auto& slot = aux->slots[j];

				if ( slot < 0 )
					continue;

				slot = frame1_to_frame2[slot];
				}
			break;
		}

		if ( inst->IsGlobalLoad() )
			{
			// Slot v2 of these is the index into globals[]
			// rather than a frame.
			int g = inst->v2;
			ASSERT(remapped_globals[g] >= 0);
			inst->v2 = remapped_globals[g];

			// We *don't* want to UpdateSlots below as
			// that's based on interpreting v2 as slots
			// rather than an index into globals.
			continue;
			}

		inst->UpdateSlots(frame1_to_frame2);

		if ( inst->IsDirectAssignment() && inst->v1 == inst->v2 )
			KillInst(inst);
		}

	frame_size = shared_frame_denizens.size();
	}

void ZAM::ReMapInterpreterFrame()
	{
	// Maps identifiers to their offset in the interpreter frame.
	std::unordered_map<const ID*, int> interpreter_slots;

	// First, track function parameters.  We could elide this
	// if we decide to alter the calling sequence for compiled
	// functions.
	auto args = scope->OrderedVars();
	auto nparam = func->FType()->Args()->NumFields();
	int next_interp_slot = 0;

	// Track old-interpreter-slots-to-new, so we can update LOAD
	// and STORE instructions.
	std::unordered_map<int, int> old_intrp_slot_to_new;

	for ( const auto& a : args )
		{
		if ( --nparam < 0 )
			break;

		ASSERT(a->Offset() == next_interp_slot);
		interpreter_slots[a.get()] = next_interp_slot;
		old_intrp_slot_to_new[a->Offset()] = next_interp_slot;
		++next_interp_slot;
		}

	for ( auto& sf : shared_frame_denizens )
		{
		// Interpreter slot to use for these shared denizens, if any.
		int cohort_slot = -1;

		// First check to see whether this cohort already has a
		// slot, which will happen if it includes a parameter.
		for ( auto& id : sf.ids )
			{
			if ( interpreter_slots.count(id) > 0 )
				{
				ASSERT(cohort_slot < 0);
				cohort_slot = interpreter_slots[id];
				}
			}

		for ( auto& id : sf.ids )
			{
			if ( interpreter_locals.count(id) == 0 )
				continue;

			if ( interpreter_slots.count(id) > 0 )
				// We already mapped this, presumably because
				// it's a parameter.
				continue;

			// Need a slot for this ID.
			if ( cohort_slot < 0 )
				// New slot.
				cohort_slot = next_interp_slot++;

			ASSERT(old_intrp_slot_to_new.count(id->Offset()) == 0);
			interpreter_slots[id] = cohort_slot;
			old_intrp_slot_to_new[id->Offset()] = cohort_slot;

			// Make the leap!
			id->SetOffset(cohort_slot);
			}
		}

	// It's conceivable that there are some locals that only live
	// in interpreter-land, depending on what sort of expressions
	// we defer to the interpreter.
	for ( auto& id : interpreter_locals )
		if ( interpreter_slots.count(id) == 0 )
			interpreter_slots[id] = next_interp_slot++;

	// Update frame sizes for functions that might have more than
	// one body.
	if ( remapped_intrp_frame_sizes.count(func) == 0 ||
	     remapped_intrp_frame_sizes[func] < next_interp_slot )
		remapped_intrp_frame_sizes[func] = next_interp_slot;

	// Rewrite references to interpreter slots to reflect remapped
	// locations.

	for ( auto i = 0; i < insts1.size(); ++i )
		{
		auto inst = insts1[i];

		if ( ! inst->live )
			continue;

		if ( inst->op_type == OP_VV_FRAME )
			{
			// All of these use v2 for the intepreter slot.
			ASSERT(old_intrp_slot_to_new.count(inst->v2) > 0);
			inst->v2 = old_intrp_slot_to_new[inst->v2];
			}
		}
	}

void ZAM::ReMapVar(ID* id, int slot, int inst)
	{
	// A greedy algorithm for this is to simply find the first suitable
	// frame slot.  We do that with one twist: we also look for a
	// compatible slot for which its current end-of-scope is exactly
	// the start-of-scope for the new identifier.  The advantage of
	// doing so is that this commonly occurs for code like "a.1 = a"
	// from resolving parameters to inlined functions, and if "a.1" and
	// "a" share the same slot then we can elide the assignment.
	//
	// In principle we could perhaps do better than greedy using a more
	// powerful allocation method like graph coloring.  However, far and
	// away the bulk of our variables are short-lived temporaries,
	// for which greedy should work fine.
	bool is_managed = IsManagedType(id->Type());

	int apt_slot = -1;
	for ( auto i = 0; i < shared_frame_denizens.size(); ++i )
		{
		auto& s = shared_frame_denizens[i];

		// Note that the following test is <= rather than <.
		// This is because assignment in instructions happens
		// after using any variables to compute the value
		// to assign.  ZAM instructions are careful to
		// allow operands and assignment destinations to
		// refer to the same slot.

		if ( s.scope_end <= inst && s.is_managed == is_managed )
			{
			// It's compatible.

			if ( s.scope_end == inst )
				{
				// It ends right on the money.
				apt_slot = i;
				break;
				}

			else if ( apt_slot < 0 )
				// We haven't found a candidate yet, take
				// this one, but keep looking.
				apt_slot = i;
			}
		}

	int scope_end = denizen_ending[slot]->inst_num;

	if ( apt_slot < 0 )
		{
		// No compatible existing slot.  Create a new one.
		apt_slot = shared_frame_denizens.size();

		FrameSharingInfo info;
		info.is_managed = is_managed;
		shared_frame_denizens.push_back(info);

		if ( is_managed )
			managed_slots.push_back(apt_slot);
		}

	auto& s = shared_frame_denizens[apt_slot];

	s.ids.push_back(id);
	s.id_start.push_back(inst);
	s.scope_end = scope_end;

	frame1_to_frame2[slot] = apt_slot;
	}

void ZAM::CheckSlotAssignment(int slot, const ZInst* inst)
	{
	ASSERT(slot >= 0 && slot < frame_denizens.size());

	// We construct temporaries such that their values are never
	// used earlier than their definitions in loop bodies.  For
	// other denizens, however, they can be, so in those cases
	// we expand the lifetime beginning to the start of any loop
	// region.
	if ( ! reducer->IsTemporary(frame_denizens[slot]) )
		inst = BeginningOfLoop(inst, 1);

	SetLifetimeStart(slot, inst);
	}

void ZAM::SetLifetimeStart(int slot, const ZInst* inst)
	{
	if ( denizen_beginning.count(slot) > 0 )
		{
		// Beginning of denizen's lifetime already seen, nothing
		// more to do other than check for consistency.
		ASSERT(denizen_beginning[slot]->inst_num <= inst->inst_num);
		}

	else
		{ // denizen begins here
		denizen_beginning[slot] = inst;

		if ( inst_beginnings.count(inst) == 0 )
			{
			// Need to create a set to track the denizens
			// beginning at the instruction.
			std::unordered_set<ID*> denizens;
			inst_beginnings[inst] = denizens;
			}

		inst_beginnings[inst].insert(frame_denizens[slot]);
		}
	}

void ZAM::CheckSlotUse(int slot, const ZInst* inst)
	{
	if ( slot < 0 )
		return;

	ASSERT(slot < frame_denizens.size());

	// See comment above about temporaries not having their values
	// extend around loop bodies.  HOWEVER if a temporary is
	// defined at a lower loop depth than that for this instruction,
	// then we extend its lifetime to the end of this instruction's
	// loop.
	if ( reducer->IsTemporary(frame_denizens[slot]) )
		{
		ASSERT(denizen_beginning.count(slot) > 0);
		int definition_depth = denizen_beginning[slot]->loop_depth;

		if ( inst->loop_depth > definition_depth )
			inst = EndOfLoop(inst, inst->loop_depth);
		}
	else
		inst = EndOfLoop(inst, 1);

	ExtendLifetime(slot, inst);
	}

void ZAM::ExtendLifetime(int slot, const ZInst* inst)
	{
	if ( denizen_ending.count(slot) > 0 )
		{
		// End of denizen's lifetime already seen.  Check for
		// consistency and then extend as needed.

		auto old_inst = denizen_ending[slot];

		// Don't complain for temporaries that already have
		// extended lifetimes, as that can happen if they're
		// used as a "for" loop-over target, which already
		// extends lifetime across the body of the loop.
		if ( inst->loop_depth > 0 &&
		     reducer->IsTemporary(frame_denizens[slot]) &&
		     old_inst->inst_num >= inst->inst_num )
			return;

		// We expect to only be extending the slot's lifetime ...
		// *unless* we're inside a nested loop, in which case 
		// the slot might have already been extended to the
		// end of the outer loop.
		ASSERT(old_inst->inst_num <= inst->inst_num ||
			inst->loop_depth > 1);

		if ( old_inst->inst_num < inst->inst_num )
			{
			// Extend.
			inst_endings[old_inst].erase(frame_denizens[slot]);

			if ( inst_endings.count(inst) == 0 )
				{
				std::unordered_set<ID*> denizens;
				inst_endings[inst] = denizens;
				}

			inst_endings[inst].insert(frame_denizens[slot]);
			denizen_ending.at(slot) = inst;
			}
		}

	else
		{ // first time seeing a use of this denizen
		denizen_ending[slot] = inst;

		if ( inst_endings.count(inst) == 0 )
			{
			std::unordered_set<ID*> denizens;
			inst_endings[inst] = denizens;
			}

		inst_endings[inst].insert(frame_denizens[slot]);
		}
	}

const ZInst* ZAM::BeginningOfLoop(const ZInst* inst, int depth) const
	{
	auto i = inst->inst_num;

	while ( i >= 0 && insts1[i]->loop_depth >= depth )
		--i;

	if ( i == inst->inst_num )
		return inst;

	// We moved backwards to just beyond a loop that inst
	// is part of.  Move to that loop's (live) beginning.
	++i;
	while ( i != inst->inst_num && ! insts1[i]->live )
		++i;

	return insts1[i];
	}

const ZInst* ZAM::EndOfLoop(const ZInst* inst, int depth) const
	{
	auto i = inst->inst_num;

	while ( i < insts1.size() && insts1[i]->loop_depth >= depth )
		++i;

	if ( i == inst->inst_num )
		return inst;

	// We moved forwards to just beyond a loop that inst
	// is part of.  Move to that loop's (live) end.
	--i;
	while ( i != inst->inst_num && ! insts1[i]->live )
		--i;

	return insts1[i];
	}

bool ZAM::VarIsAssigned(int slot) const
	{
	for ( int i = 0; i < insts1.size(); ++i )
		{
		auto& inst = insts1[i];
		if ( inst->live && VarIsAssigned(slot, inst) )
			return true;
		}

	return false;
	}

bool ZAM::VarIsAssigned(int slot, const ZInst* i) const
	{
	// Special-case for table iterators, which assign to a bunch
	// of variables but they're not immediately visible in the
	// instruction layout.
	if ( i->op == OP_NEXT_TABLE_ITER_VAL_VAR_VVV ||
	     i->op == OP_NEXT_TABLE_ITER_VV )
		{
		auto iter_vars = i->c.iter_info;
		for ( auto v : iter_vars->loop_vars )
			if ( v == slot )
				return true;

		if ( i->op != OP_NEXT_TABLE_ITER_VAL_VAR_VVV )
			return false;

		// Otherwise fall through, since that flavor of iterate
		// *does* also assign to slot 1.
		}

	if ( i->op_type == OP_VV_FRAME )
		// We don't want to consider these as assigning to the
		// variable, since the point of this method is to figure
		// out which variables don't need storing to the frame
		// because their internal value is never modified.
		return false;

	return i->AssignsToSlot1() && i->v1 == slot;
	}

bool ZAM::VarIsUsed(int slot) const
	{
	for ( int i = 0; i < insts1.size(); ++i )
		{
		auto& inst = insts1[i];
		if ( inst->live && inst->UsesSlot(slot) )
			return true;

		if ( inst->aux )
			{
			auto aux = inst->aux;
			for ( int j = 0; j < aux->n; ++j )
				if ( aux->slots[j] == slot )
					return true;
			}
		}

	return false;
	}

void ZAM::KillInst(ZInst* i)
	{
	i->live = false;
	if ( i->target )
		--(i->target->num_labels);
	if ( i->target2 )
		--(i->target2->num_labels);
	}

ZInst* ZAM::FindLiveTarget(ZInst* goto_target)
	{
	if ( goto_target == pending_inst )
		return goto_target;

	int idx = goto_target->inst_num;
	ASSERT(idx >= 0 && idx <= insts1.size());

	while ( idx < insts1.size() && ! insts1[idx]->live )
		++idx;

	if ( idx == insts1.size() )
		return pending_inst;
	else
		return insts1[idx];
	}

void ZAM::RetargetBranch(ZInst* inst, ZInst* target, int target_slot)
	{
	int t;	// instruction number of target

	if ( target == pending_inst )
		t = insts2.size();
	else
		t = target->inst_num;

	switch ( target_slot ) {
	case 1:	inst->v1 = t; break;
	case 2:	inst->v2 = t; break;
	case 3:	inst->v3 = t; break;
	case 4:	inst->v4 = t; break;

	default:
		reporter->InternalError("bad GoTo target");
	}
	}

void ZAM::StmtDescribe(ODesc* d) const
	{
	d->AddSP("compiled");
	d->AddSP(func->Name());
	}

// Unary vector operations never work on managed types, so no need
// to pass in the type ...  However, the RHS, which normally would
// be const, needs to be non-const so we can use its Type() method
// to get at a shareable VectorType.
static void vec_exec(ZOp op, VectorVal*& v1, VectorVal* v2);

// Binary ones *can* have managed types (strings).
static void vec_exec(ZOp op, BroType* t, VectorVal*& v1, VectorVal* v2,
			const VectorVal* v3);

// Vector coercion.
//
// ### Should check for underflow/overflow.
#define VEC_COERCE(tag, lhs_type, lhs_accessor, cast, rhs_accessor) \
	static VectorVal* vec_coerce_##tag(VectorVal* vec) \
		{ \
		auto& v = vec->RawVector()->ConstVec(); \
		auto yt = new VectorType(base_type(lhs_type)); \
		auto res_zv = new VectorVal(yt); \
		auto n = v.size(); \
		auto& res = res_zv->RawVector()->InitVec(n); \
		for ( unsigned int i = 0; i < n; ++i ) \
			res[i].lhs_accessor = cast(v[i].rhs_accessor); \
		return res_zv; \
		}

VEC_COERCE(IU, TYPE_INT, int_val, bro_int_t, uint_val)
VEC_COERCE(ID, TYPE_INT, int_val, bro_int_t, double_val)
VEC_COERCE(UI, TYPE_COUNT, uint_val, bro_int_t, int_val)
VEC_COERCE(UD, TYPE_COUNT, uint_val, bro_uint_t, double_val)
VEC_COERCE(DI, TYPE_DOUBLE, double_val, double, int_val)
VEC_COERCE(DU, TYPE_DOUBLE, double_val, double, uint_val)

StringVal* ZAM_to_lower(const StringVal* sv)
	{
	auto bs = sv->AsString();
	const u_char* s = bs->Bytes();
	int n = bs->Len();
	u_char* lower_s = new u_char[n + 1];
	u_char* ls = lower_s;

	for ( int i = 0; i < n; ++i )
		{
		if ( isascii(s[i]) && isupper(s[i]) )
			*ls++ = tolower(s[i]);
		else
			*ls++ = s[i];
		}

	*ls++ = '\0';
		
	return new StringVal(new BroString(1, lower_s, n));
	}

StringVal* ZAM_sub_bytes(const StringVal* s, bro_uint_t start, bro_int_t n)
	{
        if ( start > 0 )
                --start;        // make it 0-based

        BroString* ss = s->AsString()->GetSubstring(start, n);

	return new StringVal(ss ? ss : new BroString(""));
	}

double curr_CPU_time()
	{
	struct timespec ts;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
	}

IntrusivePtr<Val> ZAM::Exec(Frame* f, stmt_flow_type& flow) const
	{
	auto nv = num_Vals;
	auto ndv = num_del_Vals;

	double t = analysis_options.report_profile ? curr_CPU_time() : 0.0;

	auto val = DoExec(f, 0, flow);

	if ( analysis_options.report_profile )
		*CPU_time += curr_CPU_time() - t;

	auto dnv = num_Vals - nv;
	auto dndv = num_del_Vals - ndv;

	if ( /* dnv || dndv */ 0 )
		printf("%s vals: +%d -%d\n", func->Name(), dnv, dndv);

	return val;
	}

IntrusivePtr<Val> ZAM::DoExec(Frame* f, int start_pc,
						stmt_flow_type& flow) const
	{
	auto global_state = num_globals > 0 ? new GlobalState[num_globals] :
						nullptr;
	int pc = start_pc;
	int end_pc = insts2.size();

#define BuildVal(v, t) ZAMValUnion(v, t)
#define CopyVal(v) (IsManagedType(z.t) ? BuildVal(v.ToVal(z.t), z.t) : v)

// Managed assignments to frame[s.v1].
#define AssignV1T(v, t) { \
	if ( z.is_managed ) \
		{ \
		/* It's important to hold a reference to v here prior \
		   to the deletion in case frame[z.v1] points to v. */ \
		auto v2 = v; \
		DeleteManagedType(frame[z.v1], t); \
		frame[z.v1] = v2; \
		} \
	else \
		frame[z.v1] = v; \
	}

#define AssignV1(v) AssignV1T(v, z.t)

	// Return value, or nil if none.
	const ZAMValUnion* ret_u;

	// Type of the return value.  If nil, then we don't have a value.
	BroType* ret_type = nullptr;

	bool do_profile = analysis_options.report_profile;

	// All globals start out unloaded.
	for ( auto i = 0; i < num_globals; ++i )
		global_state[i] = GS_UNLOADED;

	ZAMValUnion* frame;

	if ( fixed_frame )
		frame = fixed_frame;
	else
		{
		frame = new ZAMValUnion[frame_size];
		// Clear slots for which we do explicit memory management.
		for ( auto s : managed_slots )
			frame[s].managed_val = nullptr;
		}

	flow = FLOW_RETURN;	// can be over-written by a Hook-Break

	while ( pc < end_pc && ! ZAM_error ) {
		auto& z = *insts2[pc];
		int profile_pc;
		double profile_CPU;

		if ( do_profile )
			{
			++ZOP_count[z.op];
			++(*inst_count)[pc];

			profile_pc = pc;
			profile_CPU = curr_CPU_time();
			}

		switch ( z.op ) {
		case OP_NOP:
			break;

#include "ZAM-OpsEvalDefs.h"
		}

		if ( do_profile )
			{
			double dt = curr_CPU_time() - profile_CPU;
			(*inst_CPU)[profile_pc] += dt;
			ZOP_CPU[z.op] += dt;
			}

		++pc;
		}

	auto result = ret_type ? ret_u->ToVal(ret_type) : nullptr;

	if ( ! fixed_frame )
		{
		// Free those slots for which we do explicit memory management.
		for ( auto i = 0; i < managed_slots.size(); ++i )
			{
			auto& v = frame[managed_slots[i]];
			DeleteManagedType(v, nullptr);
			// DeleteManagedType(v, managed_slot_types[i]);
			}

		delete [] frame;
		}

	delete [] global_state;

	// Clear any error state.
	ZAM_error = false;

	return result;
	}

#include "ZAM-OpsMethodsDefs.h"

bool ZAM::IsZAM_BuiltIn(const Expr* e)
	{
	// The expression is either directly a call (in which case there's
	// no return value), or an assignment to a call.
	const CallExpr* c;

	if ( e->Tag() == EXPR_CALL )
		c = e->AsCallExpr();
	else
		c = e->GetOp2()->AsCallExpr();

	auto func_expr = c->Func();
	if ( func_expr->Tag() != EXPR_NAME )
		return false;

	auto func_val = func_expr->AsNameExpr()->Id()->ID_Val();
	if ( ! func_val )
		return false;

	auto func = func_val->AsFunc();
	if ( func->GetKind() != BuiltinFunc::BUILTIN_FUNC )
		return false;

	auto& args = c->Args()->Exprs();

	const NameExpr* n;	// name to assign to, if any

	if ( e->Tag() == EXPR_CALL )
		n = nullptr;
	else
		n = e->GetOp1()->AsRefExpr()->GetOp1()->AsNameExpr();

	if ( streq(func->Name(), "sub_bytes") )
		return BuiltIn_sub_bytes(n, args);

	else if ( streq(func->Name(), "to_lower") )
		return BuiltIn_to_lower(n, args);

	else if ( streq(func->Name(), "Log::__write") )
		return BuiltIn_Log__write(n, args);

	else if ( streq(func->Name(), "Broker::__flush_logs") )
		return BuiltIn_Broker__flush_logs(n, args);

	else if ( streq(func->Name(), "get_port_transport_proto") )
		return BuiltIn_get_port_etc(n, args);

	else if ( streq(func->Name(), "reading_live_traffic") )
		return BuiltIn_reading_live_traffic(n, args);

	else if ( streq(func->Name(), "reading_traces") )
		return BuiltIn_reading_traces(n, args);

	else if ( streq(func->Name(), "strstr") )
		return BuiltIn_strstr(n, args);

	return false;
	}

bro_uint_t ZAM::ConstArgsMask(const expr_list& args, int nargs) const
	{
	ASSERT(args.length() == nargs);

	bro_uint_t mask = 0;

	for ( int i = 0; i < nargs; ++i )
		{
		mask <<= 1;
		if ( args[i]->Tag() == EXPR_CONST )
			mask |= 1;
		}

	return mask;
	}

bool ZAM::BuiltIn_to_lower(const NameExpr* n, const expr_list& args)
	{
	if ( ! n )
		{
		reporter->Warning("return value from built-in function ignored");
		return true;
		}

	auto arg_s = args[0]->AsNameExpr();
	int nslot = Frame1Slot(n, OP1_WRITE);

	AddInst(ZInst(OP_TO_LOWER_VV, nslot, FrameSlot(arg_s)));

	return true;
	}

bool ZAM::BuiltIn_sub_bytes(const NameExpr* n, const expr_list& args)
	{
	if ( ! n )
		{
		reporter->Warning("return value from built-in function ignored");
		return true;
		}

	auto arg_s = args[0];
	auto arg_start = args[1];
	auto arg_n = args[2];

	int nslot = Frame1Slot(n, OP1_WRITE);

	int v2 = FrameSlotIfName(arg_s);
	int v3 = ConvertToCount(arg_start);
	int v4 = ConvertToInt(arg_n);

	auto c = arg_s->Tag() == EXPR_CONST ? arg_s->AsConstExpr() : nullptr;

	ZInst z;

	switch ( ConstArgsMask(args, 3) ) {
	case 0x0:	// all variable
		z = ZInst(OP_SUB_BYTES_VVVV, nslot, v2, v3, v4);
		z.op_type = OP_VVVV;
		break;

	case 0x1:	// last argument a constant
		z = ZInst(OP_SUB_BYTES_VVVi, nslot, v2, v3, v4);
		z.op_type = OP_VVVV_I4;
		break;

	case 0x2:	// 2nd argument a constant; flip!
		z = ZInst(OP_SUB_BYTES_VViV, nslot, v2, v4, v3);
		z.op_type = OP_VVVV_I4;
		break;

	case 0x3:	// both 2nd and third are constants
		z = ZInst(OP_SUB_BYTES_VVii, nslot, v2, v3, v4);
		z.op_type = OP_VVVV_I3_I4;
		break;

	case 0x4:	// first argument a constant
		z = ZInst(OP_SUB_BYTES_VVVC, nslot, v3, v4, c);
		z.op_type = OP_VVVC;
		break;

	case 0x5:	// first and third constant
		z = ZInst(OP_SUB_BYTES_VViC, nslot, v3, v4, c);
		z.op_type = OP_VVVC_I3;
		break;

	case 0x6:	// first and second constant - flip!
		z = ZInst(OP_SUB_BYTES_ViVC, nslot, v4, v3, c);
		z.op_type = OP_VVVC_I3;
		break;

	case 0x7:	// whole shebang
		z = ZInst(OP_SUB_BYTES_ViiC, nslot, v3, v4, c);
		z.op_type = OP_VVVC_I2_I3;
		break;

	default:
		reporter->InternalError("bad constant mask");
	}

	AddInst(z);

	return true;
	}

bool ZAM::BuiltIn_Log__write(const NameExpr* n, const expr_list& args)
	{
	if ( ! log_ID_enum_type )
		{
		auto log_ID_type = lookup_ID("ID", "Log");
		ASSERT(log_ID_type);
		log_ID_enum_type = log_ID_type->Type()->AsEnumType();
		}

	auto id = args[0];
	auto columns = args[1];

	if ( columns->Tag() != EXPR_NAME )
		return false;

	auto columns_n = columns->AsNameExpr();
	auto col_slot = FrameSlot(columns_n);

	ZInst z;

	if ( n )
		{
		int nslot = Frame1Slot(n, OP1_WRITE);
		if ( id->Tag() == EXPR_CONST )
			{
			z = ZInst(OP_LOG_WRITE_VVC, nslot, col_slot,
					id->AsConstExpr());
			z.op_type = OP_VVc;
			}
		else
			z = ZInst(OP_LOG_WRITE_VVV, nslot,
					FrameSlot(id->AsNameExpr()), col_slot);
		}
	else
		{
		if ( id->Tag() == EXPR_CONST )
			{
			z = ZInst(OP_LOG_WRITE_VC, col_slot, id->AsConstExpr());
			z.op_type = OP_Vc;
			}
		else
			z = ZInst(OP_LOG_WRITE_VV, FrameSlot(id->AsNameExpr()),
					col_slot);
		}

	z.SetType(columns_n->Type());

	AddInst(z);

	return true;
	}

bool ZAM::BuiltIn_Broker__flush_logs(const NameExpr* n, const expr_list& args)
	{
	if ( n )
		AddInst(ZInst(OP_BROKER_FLUSH_LOGS_V,
				Frame1Slot(n, OP1_WRITE)));
	else
		AddInst(ZInst(OP_BROKER_FLUSH_LOGS_X));

	return true;
	}

bool ZAM::BuiltIn_get_port_etc(const NameExpr* n, const expr_list& args)
	{
	if ( ! n )
		{
		reporter->Warning("return value from built-in function ignored");
		return true;
		}

	auto p = args[0];

	if ( p->Tag() != EXPR_NAME )
		return false;

	auto pn = p->AsNameExpr();
	int nslot = Frame1Slot(n, OP1_WRITE);

	AddInst(ZInst(OP_GET_PORT_TRANSPORT_PROTO_VV, nslot, FrameSlot(pn)));

	return true;
	}

bool ZAM::BuiltIn_reading_live_traffic(const NameExpr* n, const expr_list& args)
	{
	if ( ! n )
		{
		reporter->Warning("return value from built-in function ignored");
		return true;
		}

	int nslot = Frame1Slot(n, OP1_WRITE);

	AddInst(ZInst(OP_READING_LIVE_TRAFFIC_V, nslot));

	return true;
	}

bool ZAM::BuiltIn_reading_traces(const NameExpr* n, const expr_list& args)
	{
	if ( ! n )
		{
		reporter->Warning("return value from built-in function ignored");
		return true;
		}

	int nslot = Frame1Slot(n, OP1_WRITE);

	AddInst(ZInst(OP_READING_TRACES_V, nslot));

	return true;
	}

bool ZAM::BuiltIn_strstr(const NameExpr* n, const expr_list& args)
	{
	if ( ! n )
		{
		reporter->Warning("return value from built-in function ignored");
		return true;
		}

	int nslot = Frame1Slot(n, OP1_WRITE);

	auto big = args[0];
	auto little = args[1];

	auto big_n = big->Tag() == EXPR_NAME ? big->AsNameExpr() : nullptr;
	auto little_n =
		little->Tag() == EXPR_NAME ? little->AsNameExpr() : nullptr;

	ZInst z;

	if ( big_n && little_n )
		z = GenInst(this, OP_STRSTR_VVV, n, big_n, little_n);
	else if ( big_n )
		z = GenInst(this, OP_STRSTR_VVC, n, big_n, little->AsConstExpr());
	else if ( little_n )
		z = GenInst(this, OP_STRSTR_VCV, n, little_n, big->AsConstExpr());
	else
		return false;

	AddInst(z);

	return true;
	}

const CompiledStmt ZAM::DoCall(const CallExpr* c, const NameExpr* n)
	{
	SyncGlobals(c);

	auto func = c->Func()->AsNameExpr();
	auto func_id = func->Id();
	auto& args = c->Args()->Exprs();

	int nargs = args.length();
	int call_case = nargs;

	bool indirect = ! func_id->IsGlobal();

	if ( indirect )
		call_case = -1;	// force default of CallN

	bool aux_call = true;	// whether instruction uses .aux field

	auto nt = n ? n->Type()->Tag() : TYPE_VOID;
	auto n_slot = n ? Frame1Slot(n, OP1_WRITE) : -1;

	ZInst z;

	if ( call_case == 0 )
		{
		aux_call = false;
		if ( n )
			z = ZInst(AssignmentFlavor(OP_CALL0_V, nt), n_slot);
		else
			z = ZInst(OP_CALL0_X);
		}

	else if ( call_case == 1 )
		{
		aux_call = false;
		auto arg0 = args[0];
		auto n0 = arg0->Tag() == EXPR_NAME ?
						arg0->AsNameExpr() : nullptr;
		auto c0 = arg0->Tag() == EXPR_CONST ?
						arg0->AsConstExpr() : nullptr;

		if ( n )
			{
			if ( n0 )
				z = ZInst(AssignmentFlavor(OP_CALL1_VV, nt),
						n_slot, FrameSlot(n0));
			else
				z = ZInst(AssignmentFlavor(OP_CALL1_VC, nt),
						n_slot, c0);
			}
		else
			{
			if ( n0 )
				z = ZInst(OP_CALL1_V, FrameSlot(n0));
			else
				z = ZInst(OP_CALL1_C, c0);
			}

		z.t = arg0->Type().get();
		}

	else
		{
		auto aux = new ZInstAux(nargs);

		for ( int i = 0; i < nargs; ++i )
			{
			auto ai = args[i];
			auto ai_t = ai->Type();
			if ( ai->Tag() == EXPR_NAME )
				aux->Add(i, FrameSlot(ai->AsNameExpr()), ai_t);
			else
				aux->Add(i, ai->AsConstExpr()->ValuePtr());
			}

		ZOp op;

		switch ( call_case ) {
		case 2: op = n ? OP_CALL2_Vc : OP_CALL2_c; break;
		case 3: op = n ? OP_CALL3_Vc : OP_CALL3_c; break;
		case 4: op = n ? OP_CALL4_Vc : OP_CALL4_c; break;
		case 5: op = n ? OP_CALL5_Vc : OP_CALL5_c; break;

		default:
			if ( indirect )
				op = n ? OP_INDCALLN_VVc : OP_INDCALLN_Vc;
			else
				op = n ? OP_CALLN_Vc : OP_CALLN_c;
			break;
		}

		if ( n )
			{
			op = AssignmentFlavor(op, nt);
			auto n_slot = Frame1Slot(n, OP1_WRITE);

			if ( indirect)
				{
				z = ZInst(op, n_slot, FrameSlot(func));
				z.op_type = OP_VVc;
				}

			else
				{
				z = ZInst(op, n_slot);
				z.op_type = OP_Vc;
				}
			}
		else
			{
			if ( indirect )
				{
				z = ZInst(op, FrameSlot(func));
				z.op_type = OP_Vc;
				}
			else
				{
				z = ZInst(op);
				z.op_type = OP_c;
				}
			}

		z.aux = aux;
		}

	if ( ! indirect )
		z.func = func_id->ID_Val()->AsFunc();

	if ( n )
		{
		auto id = n->Id();
		if ( id->IsGlobal() )
			{
			AddInst(z);
			z = ZInst(OP_DIRTY_GLOBAL_V, global_id_to_info[id]);
			z.op_type = OP_V_I1;
			}
		}

	return AddInst(z);
	}

void ZAM::FlushVars(const Expr* e)
	{
	ProfileFunc expr_pf;
	e->Traverse(&expr_pf);

	SyncGlobals(expr_pf.globals, e);

	for ( auto l : expr_pf.locals )
		StoreLocal(l);
	}

const CompiledStmt ZAM::ConstructTable(const NameExpr* n, const Expr* e)
	{
	auto con = e->GetOp1()->AsListExpr();
	auto tt = n->Type()->AsTableType();
	auto width = tt->Indices()->Types()->length();

	auto z = GenInst(this, OP_CONSTRUCT_TABLE_VV, n, width);
	z.aux = InternalBuildVals(con, width + 1);
	z.t = tt;
	z.attrs = e->AsTableConstructorExpr()->Attrs();

	return AddInst(z);
	}

const CompiledStmt ZAM::ConstructSet(const NameExpr* n, const Expr* e)
	{
	auto con = e->GetOp1()->AsListExpr();

	auto z = GenInst(this, OP_CONSTRUCT_SET_V, n);
	z.aux = InternalBuildVals(con);
	z.t = e->Type().get();
	z.attrs = e->AsSetConstructorExpr()->Attrs();

	return AddInst(z);
	}

const CompiledStmt ZAM::ConstructRecord(const NameExpr* n, const Expr* e)
	{
	auto con = e->GetOp1()->AsListExpr();

	auto z = GenInst(this, OP_CONSTRUCT_RECORD_V, n);
	z.aux = InternalBuildVals(con);
	z.t = e->Type().get();

	return AddInst(z);
	}

const CompiledStmt ZAM::ConstructVector(const NameExpr* n, const Expr* e)
	{
	auto con = e->GetOp1()->AsListExpr();

	auto z = GenInst(this, OP_CONSTRUCT_VECTOR_V, n);
	z.aux = InternalBuildVals(con);
	z.t = e->Type().get();

	return AddInst(z);
	}

const CompiledStmt ZAM::ArithCoerce(const NameExpr* n, const Expr* e)
	{
	auto nt = n->Type();
	auto nt_is_vec = nt->Tag() == TYPE_VECTOR;

	auto op = e->GetOp1();
	auto op_t = op->Type().get();
	auto op_is_vec = op_t->Tag() == TYPE_VECTOR;

	auto e_t = e->Type().get();
	auto et_is_vec = e_t->Tag() == TYPE_VECTOR;

	if ( nt_is_vec || op_is_vec || et_is_vec )
		{
		if ( ! (nt_is_vec && op_is_vec && et_is_vec) )
			reporter->InternalError("vector confusion compiling coercion");

		op_t = op_t->AsVectorType()->YieldType();
		e_t = e_t->AsVectorType()->YieldType();
		}

	auto targ_it = e_t->InternalType();
	auto op_it = op_t->InternalType();

	if ( op_it == targ_it )
		reporter->InternalError("coercion wasn't folded");

	if ( op->Tag() != EXPR_NAME )
		reporter->InternalError("coercion wasn't folded");

	ZOp a;

	switch ( targ_it ) {
	case TYPE_INTERNAL_DOUBLE:
		{
		a = op_it == TYPE_INTERNAL_INT ?
			(nt_is_vec ? OP_COERCE_DI_VEC_VV : OP_COERCE_DI_VV) :
			(nt_is_vec ? OP_COERCE_DU_VEC_VV : OP_COERCE_DU_VV);
		break;
		}

	case TYPE_INTERNAL_INT:
		{
		a = op_it == TYPE_INTERNAL_UNSIGNED ?
			(nt_is_vec ? OP_COERCE_IU_VEC_VV : OP_COERCE_IU_VV) :
			(nt_is_vec ? OP_COERCE_ID_VEC_VV : OP_COERCE_ID_VV);
		break;
		}

	case TYPE_INTERNAL_UNSIGNED:
		{
		a = op_it == TYPE_INTERNAL_INT ?
			(nt_is_vec ? OP_COERCE_UI_VEC_VV : OP_COERCE_UI_VV) :
			(nt_is_vec ? OP_COERCE_UD_VEC_VV : OP_COERCE_UD_VV);
		break;
		}

	default:
		reporter->InternalError("bad target internal type in coercion");
	}

	return AddInst(GenInst(this, a, n, op->AsNameExpr()));
	}

const CompiledStmt ZAM::RecordCoerce(const NameExpr* n, const Expr* e)
	{
	auto r = e->AsRecordCoerceExpr();
	auto op = r->GetOp1()->AsNameExpr();
	auto map = r->Map();
	auto map_size = r->MapSize();

	int op_slot = FrameSlot(op);
	auto zop = OP_RECORD_COERCE_VVV;
	ZInst z(zop, Frame1Slot(n, zop), op_slot, map_size);

	z.SetType(e->Type());
	z.op_type = OP_VVV_I3;
	z.int_ptr = map;

	return AddInst(z);
	}

const CompiledStmt ZAM::TableCoerce(const NameExpr* n, const Expr* e)
	{
	auto op = e->GetOp1()->AsNameExpr();

	int op_slot = FrameSlot(op);
	auto zop = OP_TABLE_COERCE_VV;
	ZInst z(zop, Frame1Slot(n, zop), op_slot);
	z.SetType(e->Type());

	return AddInst(z);
	}

const CompiledStmt ZAM::VectorCoerce(const NameExpr* n, const Expr* e)
	{
	auto op = e->GetOp1()->AsNameExpr();
	int op_slot = FrameSlot(op);

	auto zop = OP_VECTOR_COERCE_VV;
	ZInst z(zop, Frame1Slot(n, zop), op_slot);
	z.SetType(e->Type());

	return AddInst(z);
	}

const CompiledStmt ZAM::Is(const NameExpr* n, const Expr* e)
	{
	auto is = e->AsIsExpr();
	auto op = e->GetOp1()->AsNameExpr();
	int op_slot = FrameSlot(op);

	ZInst z(OP_IS_VV, Frame1Slot(n, OP_IS_VV), op_slot);
	z.e = op;
	z.SetType(is->TestType());

	return AddInst(z);
	}

const CompiledStmt ZAM::IfElse(const Expr* e, const Stmt* s1, const Stmt* s2)
	{
	CompiledStmt cond_stmt = EmptyStmt();
	int branch_v;

	if ( e->Tag() == EXPR_NAME )
		{
		auto n = e->AsNameExpr();

		ZOp op = (s1 && s2) ?
			OP_IF_ELSE_VV : (s1 ? OP_IF_VV : OP_IF_NOT_VV);

		ZInst cond(op, FrameSlot(n), 0);
		cond_stmt = AddInst(cond);
		branch_v = 2;
		}
	else
		cond_stmt = GenCond(e, branch_v);

	if ( s1 )
		{
		auto s1_end = s1->Compile(this);
		if ( s2 )
			{
			auto branch_after_s1 = GoToStub();
			auto s2_end = s2->Compile(this);
			SetV(cond_stmt, GoToTargetBeyond(branch_after_s1),
				branch_v);
			SetGoTo(branch_after_s1, GoToTargetBeyond(s2_end));

			return s2_end;
			}

		else
			{
			SetV(cond_stmt, GoToTargetBeyond(s1_end), branch_v);
			return s1_end;
			}
		}

	else
		{
		auto s2_end = s2->Compile(this);

		// For complex conditionals, we need to invert their
		// sense since we're switching to "if ( ! cond ) s2".
		auto z = insts1[cond_stmt.stmt_num];

		switch ( z->op ) {
		case OP_IF_ELSE_VV:
		case OP_IF_VV:
		case OP_IF_NOT_VV:
			// These are generated correctly above, no need
			// to fix up.
			break;

		case OP_HAS_FIELD_COND_VVV:
			z->op = OP_NOT_HAS_FIELD_COND_VVV;
			break;
		case OP_NOT_HAS_FIELD_COND_VVV:
			z->op = OP_HAS_FIELD_COND_VVV;
			break;

		case OP_VAL_IS_IN_TABLE_COND_VVV:
			z->op = OP_VAL_IS_NOT_IN_TABLE_COND_VVV;
			break;
		case OP_VAL_IS_NOT_IN_TABLE_COND_VVV:
			z->op = OP_VAL_IS_IN_TABLE_COND_VVV;
			break;

		case OP_CONST_IS_IN_TABLE_COND_VVC:
			z->op = OP_CONST_IS_NOT_IN_TABLE_COND_VVC;
			break;
		case OP_CONST_IS_NOT_IN_TABLE_COND_VVC:
			z->op = OP_CONST_IS_IN_TABLE_COND_VVC;
			break;

		case OP_VAL2_IS_IN_TABLE_COND_VVVV:
			z->op = OP_VAL2_IS_NOT_IN_TABLE_COND_VVVV;
			break;
		case OP_VAL2_IS_NOT_IN_TABLE_COND_VVVV:
			z->op = OP_VAL2_IS_IN_TABLE_COND_VVVV;
			break;

		case OP_VAL2_IS_IN_TABLE_COND_VVVC:
			z->op = OP_VAL2_IS_NOT_IN_TABLE_COND_VVVC;
			break;
		case OP_VAL2_IS_NOT_IN_TABLE_COND_VVVC:
			z->op = OP_VAL2_IS_IN_TABLE_COND_VVVC;
			break;

		case OP_VAL2_IS_IN_TABLE_COND_VVCV:
			z->op = OP_VAL2_IS_NOT_IN_TABLE_COND_VVCV;
			break;
		case OP_VAL2_IS_NOT_IN_TABLE_COND_VVCV:
			z->op = OP_VAL2_IS_IN_TABLE_COND_VVCV;
			break;

		default:
			reporter->InternalError("inconsistency in ZAM::IfElse");
		}

		SetV(cond_stmt, GoToTargetBeyond(s2_end), branch_v);
		return s2_end;
		}
	}

const CompiledStmt ZAM::GenCond(const Expr* e, int& branch_v)
	{
	auto op1 = e->GetOp1();
	auto op2 = e->GetOp2();

	NameExpr* n1 = nullptr;
	NameExpr* n2 = nullptr;
	ConstExpr* c = nullptr;

	if ( e->Tag() == EXPR_HAS_FIELD )
		{
		auto hf = e->AsHasFieldExpr();
		auto z = GenInst(this, OP_HAS_FIELD_COND_VVV, op1->AsNameExpr(),
					hf->Field());
		z.op_type = OP_VVV_I2_I3;
		branch_v = 3;
		return AddInst(z);
		}

	if ( e->Tag() == EXPR_IN )
		{
		auto op1 = e->GetOp1();
		auto op2 = e->GetOp2()->AsNameExpr();

		// First, deal with the easy cases: it's a single index.
		if ( op1->Tag() == EXPR_LIST )
			{
			auto& ind = op1->AsListExpr()->Exprs();
			if ( ind.length() == 1 )
				op1 = {NewRef{}, ind[0]};
			}

		if ( op1->Tag() == EXPR_NAME )
			{
			auto z = GenInst(this, OP_VAL_IS_IN_TABLE_COND_VVV,
						op1->AsNameExpr(), op2, 0);
			z.t = op1->Type().release();
			branch_v = 3;
			return AddInst(z);
			}

		if ( op1->Tag() == EXPR_CONST )
			{
			auto z = GenInst(this, OP_CONST_IS_IN_TABLE_COND_VVC,
						op2, op1->AsConstExpr(), 0);
			z.t = op1->Type().release();
			branch_v = 2;
			return AddInst(z);
			}

		// Now the harder case: 2 indexes.  (Any number here other
		// than two should have been disallowed due to how we reduce
		// conditional expressions.)

		auto& ind = op1->AsListExpr()->Exprs();
		ASSERT(ind.length() == 2);

		auto ind0 = ind[0];
		auto ind1 = ind[1];

		auto name0 = ind0->Tag() == EXPR_NAME;
		auto name1 = ind1->Tag() == EXPR_NAME;

		auto n0 = name0 ? ind0->AsNameExpr() : nullptr;
		auto n1 = name1 ? ind1->AsNameExpr() : nullptr;

		auto c0 = name0 ? nullptr : ind0->AsConstExpr();
		auto c1 = name1 ? nullptr : ind1->AsConstExpr();

		ZInst z;

		if ( name0 && name1 )
			{
			z = GenInst(this, OP_VAL2_IS_IN_TABLE_COND_VVVV,
					n0, n1, op2, 0);
			branch_v = 4;
			z.t = n0->Type().release();
			}

		else if ( name0 )
			{
			z = GenInst(this, OP_VAL2_IS_IN_TABLE_COND_VVVC,
					n0, op2, c1, 0);
			branch_v = 3;
			z.t = n0->Type().release();
			}

		else if ( name1 )
			{
			z = GenInst(this, OP_VAL2_IS_IN_TABLE_COND_VVCV,
					n1, op2, c0, 0);
			branch_v = 3;
			z.t = n1->Type().release();
			}

		else
			{ // Both are constants, assign first to temporary.
			auto slot = NewSlot(c0->Type());
			auto z = ZInst(OP_ASSIGN_CONST_VC, slot, c0);
			z.CheckIfManaged(c0);
			(void) AddInst(z);

			z = ZInst(OP_VAL2_IS_IN_TABLE_COND_VVVC,
					slot, FrameSlot(op2), 0, c1);
			z.op_type = OP_VVVC_I3;
			z.t = c0->Type().release();
			}

		return AddInst(z);
		}

	if ( op1->Tag() == EXPR_NAME )
		{
		n1 = op1->AsNameExpr();

		if ( op2->Tag() == EXPR_NAME )
			n2 = op2->AsNameExpr();
		else
			c = op2->AsConstExpr();
		}

	else
		{
		c = op1->AsConstExpr();
		n2 = op2->AsNameExpr();
		}

	if ( n1 && n2 )
		branch_v = 3;
	else
		branch_v = 2;

	switch ( e->Tag() ) {
#include "ZAM-Conds.h"

	default:
		reporter->InternalError("bad expression type in ZAM::GenCond");
	}

	// Not reached.
	}

const CompiledStmt ZAM::While(const Stmt* cond_stmt, const Expr* cond,
				const Stmt* body)
	{
	auto head = StartingBlock();

	if ( cond_stmt )
		(void) cond_stmt->Compile(this);

	CompiledStmt cond_IF = EmptyStmt();
	int branch_v;

	if ( cond->Tag() == EXPR_NAME )
		{
		auto n = cond->AsNameExpr();
		cond_IF = AddInst(ZInst(OP_IF_VV, FrameSlot(n), 0));
		branch_v = 2;
		}
	else
		cond_IF = GenCond(cond, branch_v);

	PushNexts();
	PushBreaks();

	if ( body && body->Tag() != STMT_NULL )
		(void) body->Compile(this);

	auto tail = GoTo(GoToTarget(head));

	auto beyond_tail = GoToTargetBeyond(tail);
	SetV(cond_IF, beyond_tail, branch_v);

	ResolveNexts(GoToTarget(head));
	ResolveBreaks(beyond_tail);

	return tail;
	}

const CompiledStmt ZAM::Loop(const Stmt* body)
	{
	PushNexts();
	PushBreaks();

	auto head = StartingBlock();
	(void) body->Compile(this);
	auto tail = GoTo(GoToTarget(head));

	ResolveNexts(GoToTarget(head));
	ResolveBreaks(GoToTargetBeyond(tail));

	return tail;
	}

const CompiledStmt ZAM::When(Expr* cond, const Stmt* body,
				const Expr* timeout, const Stmt* timeout_body,
				bool is_return)
	{
	// ### Flush locals on eval, and also on exit
	ZInst z;

	if ( timeout )
		{
		// Note, we fill in is_return by hand since it's already
		// an int_val, doesn't need translation.
		if ( timeout->Tag() == EXPR_CONST )
			{
			z = GenInst(this, OP_WHEN_VVVC, timeout->AsConstExpr());
			z.op_type = OP_VVVC_I1_I2_I3;
			z.v3 = is_return;
			}
		else
			{
			z = GenInst(this, OP_WHEN_VVVV, timeout->AsNameExpr());
			z.op_type = OP_VVVV_I2_I3_I4;
			z.v4 = is_return;
			}
		}

	else
		{
		z = GenInst(this, OP_WHEN_VV);
		z.op_type = OP_VV_I1_I2;
		z.v1 = is_return;
		}

	z.non_const_e = cond;

	auto when_eval = AddInst(z);

	auto branch_past_blocks = GoToStub();

	auto when_body = body->Compile(this);
	auto when_done = ReturnX();

	if ( timeout )
		{
		auto t_body = timeout_body->Compile(this);
		auto t_done = ReturnX();

		if ( timeout->Tag() == EXPR_CONST )
			{
			SetV1(when_eval, GoToTargetBeyond(branch_past_blocks));
			SetV2(when_eval, GoToTargetBeyond(when_done));
			}
		else
			{
			SetV2(when_eval, GoToTargetBeyond(branch_past_blocks));
			SetV3(when_eval, GoToTargetBeyond(when_done));
			}

		SetGoTo(branch_past_blocks, GoToTargetBeyond(t_done));

		return t_done;
		}

	else
		{
		SetV2(when_eval, GoToTargetBeyond(branch_past_blocks));
		SetGoTo(branch_past_blocks, GoToTargetBeyond(when_done));

		return when_done;
		}
	}

const CompiledStmt ZAM::Switch(const SwitchStmt* sw)
	{
	auto e = sw->StmtExpr();

	const NameExpr* n = e->Tag() == EXPR_NAME ? e->AsNameExpr() : nullptr;
	const ConstExpr* c = e->Tag() == EXPR_CONST ? e->AsConstExpr() : nullptr;

	auto t = e->Type()->Tag();

	PushBreaks();

	if ( t != TYPE_ANY && t != TYPE_TYPE )
		return ValueSwitch(sw, n, c);
	else
		return TypeSwitch(sw, n, c);
	}

const CompiledStmt ZAM::ValueSwitch(const SwitchStmt* sw, const NameExpr* v,
					const ConstExpr* c)
	{
	int slot = v ? FrameSlot(v) : 0;

	if ( c )
		{
		// Weird to have a constant switch expression, enough
		// so that it doesn't seem worth optimizing.
		slot = NewSlot(c->Type());
		auto z = ZInst(OP_ASSIGN_CONST_VC, slot, c);
		z.CheckIfManaged(c);
		(void) AddInst(z);
		}

	// Figure out which jump table we're using.
	auto t = v ? v->Type() : c->Type();
	int tbl = 0;
	ZOp op;

	switch ( t->InternalType() ) {
	case TYPE_INTERNAL_INT:
		op = OP_SWITCHI_VVV;
		tbl = int_cases.size();
		break;

	case TYPE_INTERNAL_UNSIGNED:
		op = OP_SWITCHU_VVV;
		tbl = uint_cases.size();
		break;

	case TYPE_INTERNAL_DOUBLE:
		op = OP_SWITCHD_VVV;
		tbl = double_cases.size();
		break;

	case TYPE_INTERNAL_STRING:
		op = OP_SWITCHS_VVV;
		tbl = str_cases.size();
		break;

	case TYPE_INTERNAL_ADDR:
		op = OP_SWITCHA_VVV;
		tbl = str_cases.size();
		break;

	case TYPE_INTERNAL_SUBNET:
		op = OP_SWITCHN_VVV;
		tbl = str_cases.size();
		break;

	default:
		reporter->InternalError("bad switch type");
	}

	// Add the "head", i.e., the execution of the jump table.
	auto sw_head_op = ZInst(op, slot, tbl, 0);
	sw_head_op.op_type = OP_VVV_I2_I3;

	auto sw_head = AddInst(sw_head_op);
	auto body_end = sw_head;

	// Generate each of the cases.
	auto cases = sw->Cases();
	std::vector<InstLabel> case_start;

	PushFallThroughs();
	for ( auto c : *cases )
		{
		auto start = GoToTargetBeyond(body_end);
		ResolveFallThroughs(start);
		case_start.push_back(start);
		PushFallThroughs();
		body_end = c->Body()->Compile(this);
		}

	auto sw_end = GoToTargetBeyond(body_end);
	ResolveFallThroughs(sw_end);
	ResolveBreaks(sw_end);

	int def_ind = sw->DefaultCaseIndex();
	if ( def_ind >= 0 )
		SetV3(sw_head, case_start[def_ind]);
	else
		SetV3(sw_head, sw_end);

	// Now fill out the corresponding jump table.
	//
	// We will only use one of these.
	CaseMap<bro_int_t> new_int_cases;
	CaseMap<bro_uint_t> new_uint_cases;
	CaseMap<double> new_double_cases;
	CaseMap<std::string> new_str_cases;

	auto val_map = sw->ValueMap();

	// Ugh: the switch statement data structures don't store
	// the values directly, so we have to back-scrape them from
	// the interpreted jump table.
	auto ch = sw->CompHash();

	HashKey* k;
	int* index;
	IterCookie* cookie = val_map->InitForIteration();
	while ( (index = val_map->NextEntry(k, cookie)) )
		{
		auto case_val_list = ch->RecoverVals(k);
		delete k;

		auto case_vals = case_val_list->Vals();

		if ( case_vals->length() != 1 )
			reporter->InternalError("bad recovered value when compiling switch");

		auto cv = (*case_vals)[0];
		auto case_body_start = case_start[*index];

		switch ( cv->Type()->InternalType() ) {
		case TYPE_INTERNAL_INT:
			new_int_cases[cv->InternalInt()] = case_body_start;
			break;

		case TYPE_INTERNAL_UNSIGNED:
			new_uint_cases[cv->InternalUnsigned()] = case_body_start;
			break;

		case TYPE_INTERNAL_DOUBLE:
			new_double_cases[cv->InternalDouble()] = case_body_start;
			break;

		case TYPE_INTERNAL_STRING:
			{
			// This leaks, but only statically so not worth
			// tracking the value for ultimate deletion.
			auto sv = cv->AsString()->Render();
			std::string s(sv);
			new_str_cases[s] = case_body_start;
			break;
			}

		case TYPE_INTERNAL_ADDR:
			{
			auto a = cv->AsAddr().AsString();
			new_str_cases[a] = case_body_start;
			break;
			}

		case TYPE_INTERNAL_SUBNET:
			{
			auto n = cv->AsSubNet().AsString();
			new_str_cases[n] = case_body_start;
			break;
			}

		default:
			reporter->InternalError("bad recovered type when compiling switch");
		}
		}

	// Now add the jump table to the set we're keeping for the
	// corresponding type.

	switch ( t->InternalType() ) {
	case TYPE_INTERNAL_INT:
		int_cases.push_back(new_int_cases);
		break;

	case TYPE_INTERNAL_UNSIGNED:
		uint_cases.push_back(new_uint_cases);
		break;

	case TYPE_INTERNAL_DOUBLE:
		double_cases.push_back(new_double_cases);
		break;

	case TYPE_INTERNAL_STRING:
	case TYPE_INTERNAL_ADDR:
	case TYPE_INTERNAL_SUBNET:
		str_cases.push_back(new_str_cases);
		break;

	default:
		reporter->InternalError("bad switch type");
	}

	return body_end;
	}

const CompiledStmt ZAM::TypeSwitch(const SwitchStmt* sw, const NameExpr* v,
					const ConstExpr* c)
	{
	auto cases = sw->Cases();
	auto type_map = sw->TypeMap();

	auto body_end = EmptyStmt();

	auto tmp = NewSlot(true);	// true since we know "any" is managed

	int slot = v ? FrameSlot(v) : 0;

	if ( v && v->Type()->Tag() != TYPE_ANY )
		{
		auto z = ZInst(OP_ASSIGN_ANY_VV, tmp, slot);
		body_end = AddInst(z);
		slot = tmp;
		}

	if ( c )
		{
		auto z = ZInst(OP_ASSIGN_ANY_VC, tmp, c);
		body_end = AddInst(z);
		slot = tmp;
		}

	int def_ind = sw->DefaultCaseIndex();
	CompiledStmt def_succ(0);	// successor to default, if any
	bool saw_def_succ = false;	// whether def_succ is meaningful

	PushFallThroughs();
	for ( auto& i : *type_map )
		{
		auto id = i.first;
		auto type = id->Type();

		ZInst z;

		z = ZInst(OP_BRANCH_IF_NOT_TYPE_VV, slot, 0);
		z.SetType(type);
		auto case_test = AddInst(z);

		// Type cases that don't use "as" create a placeholder
		// ID with a null name.
		if ( id->Name() )
			{
			int id_slot = Frame1Slot(id, OP_CAST_ANY_VV);
			z = ZInst(OP_CAST_ANY_VV, id_slot, slot);
			z.SetType(type);
			body_end = AddInst(z);
			}
		else
			body_end = case_test;

		ResolveFallThroughs(GoToTargetBeyond(body_end));
		body_end = (*cases)[i.second]->Body()->Compile(this);
		SetV2(case_test, GoToTargetBeyond(body_end));

		if ( def_ind >= 0 && i.second == def_ind + 1 )
			{
			def_succ = case_test;
			saw_def_succ = true;
			}

		PushFallThroughs();
		}

	ResolveFallThroughs(GoToTargetBeyond(body_end));

	if ( def_ind >= 0 )
		{
		PushFallThroughs();

		body_end = (*sw->Cases())[def_ind]->Body()->Compile(this);

		// Now resolve any fallthrough's in the default.
		if ( saw_def_succ )
			ResolveFallThroughs(GoToTargetBeyond(def_succ));
		else
			ResolveFallThroughs(GoToTargetBeyond(body_end));
		}

	ResolveBreaks(GoToTargetBeyond(body_end));

	return body_end;
	}

const CompiledStmt ZAM::For(const ForStmt* f)
	{
	auto e = f->LoopExpr();
	auto val = e->AsNameExpr();
	auto et = e->Type()->Tag();

	PushNexts();
	PushBreaks();

	if ( et == TYPE_TABLE )
		return LoopOverTable(f, val);

	else if ( et == TYPE_VECTOR )
		return LoopOverVector(f, val);

	else if ( et == TYPE_STRING )
		return LoopOverString(f, val);

	else
		reporter->InternalError("bad \"for\" loop-over value when compiling");
	}

const CompiledStmt ZAM::Call(const ExprStmt* e)
	{
	if ( IsZAM_BuiltIn(e->StmtExpr()) )
		return LastInst();

	auto call = e->StmtExpr()->AsCallExpr();
	return DoCall(call, nullptr);
	}

const CompiledStmt ZAM::AssignToCall(const ExprStmt* e)
	{
	if ( IsZAM_BuiltIn(e->StmtExpr()) )
		return LastInst();

	auto assign = e->StmtExpr()->AsAssignExpr();
	auto n = assign->GetOp1()->AsRefExpr()->GetOp1()->AsNameExpr();
	auto call = assign->GetOp2()->AsCallExpr();

	return DoCall(call, n);
	}

const CompiledStmt ZAM::AssignVecElems(const Expr* e)
	{
	auto index_assign = e->AsIndexAssignExpr();

	auto op1 = index_assign->GetOp1();
	auto op3 = index_assign->GetOp3();
	auto any_val = IsAny(op3->Type());

	auto lhs = op1->AsNameExpr();
	auto lt = lhs->Type();

	if ( IsAnyVec(lt) )
		{
		ZInst z;

		if ( any_val )
			// No need to set the type, as it's retrieved
			// dynamically.
			z = GenInst(this, OP_TRANSFORM_ANY_VEC2_VV, lhs,
					op3->AsNameExpr());
		else
			{
			z = GenInst(this, OP_TRANSFORM_ANY_VEC_V, lhs);
			z.SetType(op3->Type());
			}

		AddInst(z);
		}

	auto indexes_expr = index_assign->GetOp2()->AsListExpr();
	auto indexes = indexes_expr->Exprs();

	if ( indexes.length() > 1 )
		{ // Vector slice assignment.
		ASSERT(op1->Tag() == EXPR_NAME);
		ASSERT(op3->Tag() == EXPR_NAME);
		ASSERT(op1->Type()->Tag() == TYPE_VECTOR);
		ASSERT(op3->Type()->Tag() == TYPE_VECTOR);

		auto z = GenInst(this, OP_VECTOR_SLICE_ASSIGN_VV,
					op1->AsNameExpr(), op3->AsNameExpr());

		z.aux = InternalBuildVals(indexes_expr);

		return AddInst(z);
		}

	auto op2 = indexes[0];

	if ( op2->Tag() == EXPR_CONST && op3->Tag() == EXPR_CONST )
		{
		// Turn into a VVC assignment by assigning the index to
		// a temporary.
		auto c = op2->AsConstExpr();
		auto tmp = NewSlot(c->Type());
		auto z = ZInst(OP_ASSIGN_CONST_VC, tmp, c);
		z.CheckIfManaged(c);

		AddInst(z);

		auto zop = OP_VECTOR_ELEM_ASSIGN_VVC;

		return AddInst(ZInst(zop, Frame1Slot(lhs, zop), tmp,
					op3->AsConstExpr()));
		}

	if ( op2->Tag() == EXPR_NAME )
		{
		CompiledStmt inst(0);

		if ( op3->Tag() == EXPR_NAME )
			inst = any_val ? Vector_Elem_Assign_AnyVVV(lhs,
							op2->AsNameExpr(),
							op3->AsNameExpr()) :
					Vector_Elem_AssignVVV(lhs,
							op2->AsNameExpr(),
							op3->AsNameExpr());
		else
			inst = Vector_Elem_AssignVVC(lhs, op2->AsNameExpr(),
							op3->AsConstExpr());

		TopMainInst()->t = op3->Type().get();
		return inst;
		}

	else
		{
		auto c = op2->AsConstExpr();
		auto index = c->Value()->AsCount();

		auto inst = any_val ? Vector_Elem_Assign_AnyVVi(lhs,
						op3->AsNameExpr(), index) :
					Vector_Elem_AssignVVi(lhs,
						op3->AsNameExpr(), index);

		TopMainInst()->t = op3->Type().get();
		return inst;
		}
	}

const CompiledStmt ZAM::AssignTableElem(const Expr* e)
	{
	auto index_assign = e->AsIndexAssignExpr();

	auto op1 = index_assign->GetOp1()->AsNameExpr();
	auto op2 = index_assign->GetOp2()->AsListExpr();
	auto op3 = index_assign->GetOp3();

	ZInst z;

	if ( op3->Tag() == EXPR_NAME )
		z = GenInst(this, OP_TABLE_ELEM_ASSIGN_VV,
				op1, op3->AsNameExpr());
	else
		z = GenInst(this, OP_TABLE_ELEM_ASSIGN_VC,
				op1, op3->AsConstExpr());

	z.aux = InternalBuildVals(op2);
	z.t = op3->Type().get();

	return AddInst(z);
	}

const CompiledStmt ZAM::LoopOverTable(const ForStmt* f, const NameExpr* val)
	{
	auto loop_vars = f->LoopVars();
	auto value_var = f->ValueVar();

	auto ii = new IterInfo();

	for ( int i = 0; i < loop_vars->length(); ++i )
		{
		auto id = (*loop_vars)[i];
		ii->loop_vars.push_back(FrameSlot(id));
		ii->loop_var_types.push_back(id->Type());
		}

	ZAMValUnion ii_val;
	ii_val.iter_info = ii;

	auto info = NewSlot(false);	// false <- IterInfo isn't managed
	auto z = ZInst(OP_INIT_TABLE_LOOP_VVC, info, FrameSlot(val));
	z.c = ii_val;
	z.op_type = OP_VVc;
	z.SetType(value_var ? value_var->Type() : nullptr);
	auto init_end = AddInst(z);

	auto iter_head = StartingBlock();
	if ( value_var )
		{
		z = ZInst(OP_NEXT_TABLE_ITER_VAL_VAR_VVV, FrameSlot(value_var),
				info, 0);
		z.c = ii_val;
		z.CheckIfManaged(value_var->Type());
		z.op_type = OP_VVV_I3;
		}
	else
		{
		z = ZInst(OP_NEXT_TABLE_ITER_VV, info, 0);
		z.c = ii_val;
		z.op_type = OP_VV_I2;
		}

	return FinishLoop(iter_head, z, f->LoopBody(), info);
	}

const CompiledStmt ZAM::LoopOverVector(const ForStmt* f, const NameExpr* val)
	{
	auto loop_vars = f->LoopVars();
	auto loop_var = (*loop_vars)[0];

	auto ii = new IterInfo();
	ii->vec_type = val->Type()->AsVectorType();
	ii->yield_type = ii->vec_type->YieldType();

	ZAMValUnion ii_val;
	ii_val.iter_info = ii;

	auto info = NewSlot(false);
	auto z = ZInst(OP_INIT_VECTOR_LOOP_VV, info, FrameSlot(val));
	z.c = ii_val;
	z.op_type = OP_VVc;

	auto init_end = AddInst(z);

	auto iter_head = StartingBlock();

	z = ZInst(OP_NEXT_VECTOR_ITER_VVV, FrameSlot(loop_var), info, 0);
	z.op_type = OP_VVV_I3;

	return FinishLoop(iter_head, z, f->LoopBody(), info);
	}

const CompiledStmt ZAM::LoopOverString(const ForStmt* f, const NameExpr* val)
	{
	auto loop_vars = f->LoopVars();
	auto loop_var = (*loop_vars)[0];

	ZAMValUnion ii_val;
	ii_val.iter_info = new IterInfo();

	auto info = NewSlot(false);
	auto z = ZInst(OP_INIT_STRING_LOOP_VV, info, FrameSlot(val));
	z.c = ii_val;
	z.op_type = OP_VVc;

	auto init_end = AddInst(z);

	auto iter_head = StartingBlock();

	z = ZInst(OP_NEXT_STRING_ITER_VVV, FrameSlot(loop_var), info, 0);
	z.CheckIfManaged(loop_var->Type());
	z.op_type = OP_VVV_I3;

	return FinishLoop(iter_head, z, f->LoopBody(), info);
	}

const CompiledStmt ZAM::FinishLoop(const CompiledStmt iter_head,
					ZInst iter_stmt, const Stmt* body,
					int info_slot)
	{
	auto loop_iter = AddInst(iter_stmt);
	auto body_end = body->Compile(this);

	auto loop_end = GoTo(GoToTarget(iter_head));
	auto final_stmt = AddInst(ZInst(OP_END_LOOP_V, info_slot));

	if ( iter_stmt.op_type == OP_VVV_I3 )
		SetV3(loop_iter, GoToTarget(final_stmt));
	else
		SetV2(loop_iter, GoToTarget(final_stmt));

	ResolveNexts(GoToTarget(iter_head));
	ResolveBreaks(GoToTarget(final_stmt));

	return final_stmt;
	}

const CompiledStmt ZAM::InitRecord(ID* id, RecordType* rt)
	{
	auto z = ZInst(OP_INIT_RECORD_V, FrameSlot(id));
	z.SetType(rt);
	return AddInst(z);
	}

const CompiledStmt ZAM::InitVector(ID* id, VectorType* vt)
	{
	auto z = ZInst(OP_INIT_VECTOR_V, FrameSlot(id));
	z.SetType(vt);
	return AddInst(z);
	}

const CompiledStmt ZAM::InitTable(ID* id, TableType* tt, Attributes* attrs)
	{
	auto z = ZInst(OP_INIT_TABLE_V, FrameSlot(id));
	z.SetType(tt);
	z.attrs = attrs;
	return AddInst(z);
	}

const CompiledStmt ZAM::Return(const ReturnStmt* r)
	{
	auto e = r->StmtExpr();

	// We could consider only doing this sync for "true" returns
	// and not for catch-return's.  To make that work, however,
	// would require propagating the "dirty" status of globals
	// modified inside an inlined function.  These changes aren't
	// visible because RDs don't propagate across return's, even
	// inlined ones.  See the coment in for STMT_RETURN's in
	// RD_Decorate::PostStmt for why we can't simply propagate
	// RDs in this case.
	//
	// In addition, by sync'ing here rather than deferring we
	// provide opportunities to double-up the frame slot used
	// by the global.
	SyncGlobals(r);

	if ( retvars.size() == 0 )
		{ // a "true" return
		if ( e )
			{
			if ( e->Tag() == EXPR_NAME )
				return ReturnV(e->AsNameExpr());
			else
				return ReturnC(e->AsConstExpr());
			}

		else
			return ReturnX();
		}

	auto rv = retvars.back();
	if ( e && ! rv )
		reporter->InternalError("unexpected returned value inside inlined block");
	if ( ! e && rv )
		reporter->InternalError("expected returned value inside inlined block but none provider");

	if ( e )
		{
		if ( e->Tag() == EXPR_NAME )
			(void) AssignXV(rv, e->AsNameExpr());
		else
			(void) AssignXC(rv, e->AsConstExpr());
		}

	return CatchReturn();
	}

const CompiledStmt ZAM::CatchReturn(const CatchReturnStmt* cr)
	{
	retvars.push_back(cr->RetVar());

	PushCatchReturns();

	auto block_end = cr->Block()->Compile(this);
	retvars.pop_back();

	ResolveCatchReturns(GoToTargetBeyond(block_end));

	return block_end;
	}

const CompiledStmt ZAM::StartingBlock()
	{
	return CompiledStmt(insts1.size());
	}

const CompiledStmt ZAM::FinishBlock(const CompiledStmt /* start */)
	{
	return CompiledStmt(insts1.size() - 1);
	}

bool ZAM::NullStmtOK() const
	{
	// They're okay iff they're the entire statement body.
	return insts1.size() == 0;
	}

const CompiledStmt ZAM::EmptyStmt()
	{
	return CompiledStmt(insts1.size() - 1);
	}

const CompiledStmt ZAM::LastInst()
	{
	return CompiledStmt(insts1.size() - 1);
	}

const CompiledStmt ZAM::ErrorStmt()
	{
	error_seen = true;
	return CompiledStmt(0);
	}

bool ZAM::IsUnused(const ID* id, const Stmt* where) const
	{
	if ( ! ud->HasUsage(where) )
		return true;

	return ! ud->GetUsage(where)->HasID(id);
	}

OpaqueVals* ZAM::BuildVals(const IntrusivePtr<ListExpr>& l)
	{
	return new OpaqueVals(InternalBuildVals(l.get()));
	}

ZInstAux* ZAM::InternalBuildVals(const ListExpr* l, int stride)
	{
	auto exprs = l->Exprs();
	int n = exprs.length();

	auto aux = new ZInstAux(n * stride);

	int offset = 0;	// offset into aux info
	for ( int i = 0; i < n; ++i )
		{
		auto& e = exprs[i];
		int num_vals = InternalAddVal(aux, offset, e);
		ASSERT(num_vals == stride);
		offset += num_vals;
		}

	return aux;
	}

int ZAM::InternalAddVal(ZInstAux* zi, int i, Expr* e)
	{
	if ( e->Tag() == EXPR_ASSIGN )
		{ // We're building up a table constructor
		auto& indices = e->GetOp1()->AsListExpr()->Exprs();
		auto val = e->GetOp2();
		int width = indices.length();

		for ( int j = 0; j < width; ++j )
			ASSERT(InternalAddVal(zi, i + j, indices[j]) == 1);

		ASSERT(InternalAddVal(zi, i + width, val.get()) == 1);

		return width + 1;
		}

	if ( e->Tag() == EXPR_FIELD_ASSIGN )
		{
		// These can appear when we're processing the
		// expression list for a record constructor.
		auto fa = e->AsFieldAssignExpr();
		e = fa->GetOp1().get();

		if ( e->Type()->Tag() == TYPE_TYPE )
			{
			// Ugh - we actually need a "type" constant.
			auto v = e->Eval(nullptr);
			ASSERT(v);
			zi->Add(i, v);
			return 1;
			}

		// Now that we've adjusted, fall through.
		}

	if ( e->Tag() == EXPR_NAME )
		zi->Add(i, FrameSlot(e->AsNameExpr()), e->Type());

	else
		zi->Add(i, e->AsConstExpr()->ValuePtr());

	return 1;
	}

const CompiledStmt ZAM::AddInst(const ZInst& inst)
	{
	ZInst* i;

	if ( pending_inst )
		{
		i = pending_inst;
		pending_inst = nullptr;
		}
	else
		i = new ZInst();

	*i = inst;

	insts1.push_back(i);

	top_main_inst = insts1.size() - 1;

	if ( mark_dirty < 0 )
		return CompiledStmt(top_main_inst);

	auto dirty_global_slot = mark_dirty;
	mark_dirty = -1;

	auto dirty_inst = ZInst(OP_DIRTY_GLOBAL_V, dirty_global_slot);
	dirty_inst.op_type = OP_V_I1;

	return AddInst(dirty_inst);
	}

const Stmt* ZAM::LastStmt() const
	{
	if ( body->Tag() == STMT_LIST )
		{
		auto sl = body->AsStmtList()->Stmts();
		return sl[sl.length() - 1];
		}

	else
		return body;
	}

const CompiledStmt ZAM::LoadOrStoreLocal(ID* id, bool is_load, bool add)
	{
	if ( id->AsType() )
		reporter->InternalError("don't know how to compile local variable that's a type not a value");

	if ( ! is_load )
		interpreter_locals.insert(id);

	bool is_any = IsAny(id->Type());

	ZOp op;

	if ( is_load )
		op = AssignmentFlavor(OP_LOAD_VAL_VV, id->Type()->Tag());
	else
		op = is_any ? OP_STORE_ANY_VAL_VV : OP_STORE_VAL_VV;

	int slot = (is_load && add) ? AddToFrame(id) : FrameSlot(id);

	ZInst z(op, slot, id->Offset());
	z.SetType(id->Type());
	z.op_type = OP_VV_FRAME;

	return AddInst(z);
	}

const CompiledStmt ZAM::LoadGlobal(ID* id)
	{
	if ( id->AsType() )
		// We never operate on these directly, so don't bother
		// storing or loading them.
		return EmptyStmt();

	ZOp op = AssignmentFlavor(OP_LOAD_GLOBAL_VVC, id->Type()->Tag());

	auto slot = RawSlot(id);

	ZInst z(op, slot, global_id_to_info[id]);
	z.c.id_val = id;
	z.SetType(id->Type());
	z.op_type = OP_ViC_ID;

	return AddInst(z);
	}

int ZAM::AddToFrame(ID* id)
	{
	frame_layout1[id] = frame_size;
	frame_denizens.push_back(id);
	return frame_size++;
	}

void ZAM::ProfileExecution() const
	{
	if ( inst_count->size() == 0 )
		{
		printf("%s has an empty body\n", func->Name());
		return;
		}

	if ( (*inst_count)[0] == 0 )
		{
		printf("%s did not execute\n", func->Name());
		return;
		}

	printf("%s CPU time: %.06f\n", func->Name(), *CPU_time);

	for ( int i = 0; i < inst_count->size(); ++i )
		{
		printf("%s %d %d %.06f ", func->Name(), i,
			(*inst_count)[i], (*inst_CPU)[i]);
		insts2[i]->Dump(&frame_denizens, &shared_frame_denizens_final);
		}
	}

void ZAM::Dump()
	{
	bool remapped_frame = ! analysis_options.no_ZAM_opt;

	if ( remapped_frame )
		printf("Original frame:\n");

	for ( auto frame_elem : frame_layout1 )
		printf("frame[%d] = %s\n", frame_elem.second, frame_elem.first->Name());

	if ( remapped_frame )
		{
		printf("Final frame:\n");

		for ( auto i = 0; i < shared_frame_denizens.size(); ++i )
			{
			printf("frame2[%d] =", i);
			for ( auto& id : shared_frame_denizens[i].ids )
				printf(" %s", id->Name());
			printf("\n");
			}
		}

	if ( insts2.size() > 0 )
		printf("Pre-removal of dead code:\n");

	auto remappings = remapped_frame ? &shared_frame_denizens : nullptr;

	for ( int i = 0; i < insts1.size(); ++i )
		{
		auto& inst = insts1[i];
		auto depth = inst->loop_depth;
		printf("%d%s%s: ", i, inst->live ? "" : " (dead)",
			depth ? fmt(" (loop %d)", depth) : "");
		inst->Dump(&frame_denizens, remappings);
		}

	if ( insts2.size() > 0 )
		printf("Final code:\n");

	remappings = remapped_frame ? &shared_frame_denizens_final : nullptr;

	for ( int i = 0; i < insts2.size(); ++i )
		{
		auto& inst = insts2[i];
		auto depth = inst->loop_depth;
		printf("%d%s%s: ", i, inst->live ? "" : " (dead)",
			depth ? fmt(" (loop %d)", depth) : "");
		inst->Dump(&frame_denizens, remappings);
		}

	for ( int i = 0; i < int_cases.size(); ++i )
		DumpIntCases(i);
	for ( int i = 0; i < uint_cases.size(); ++i )
		DumpUIntCases(i);
	for ( int i = 0; i < double_cases.size(); ++i )
		DumpDoubleCases(i);
	for ( int i = 0; i < str_cases.size(); ++i )
		DumpStrCases(i);
	}

void ZAM::DumpIntCases(int i) const
	{
	printf("int switch table #%d:", i);
	for ( auto& m : int_cases[i] )
		printf(" %lld->%d", m.first, m.second->inst_num);
	printf("\n");
	}

void ZAM::DumpUIntCases(int i) const
	{
	printf("uint switch table #%d:", i);
	for ( auto& m : uint_cases[i] )
		printf(" %llu->%d", m.first, m.second->inst_num);
	printf("\n");
	}

void ZAM::DumpDoubleCases(int i) const
	{
	printf("double switch table #%d:", i);
	for ( auto& m : double_cases[i] )
		printf(" %lf->%d", m.first, m.second->inst_num);
	printf("\n");
	}

void ZAM::DumpStrCases(int i) const
	{
	printf("str switch table #%d:", i);
	for ( auto& m : str_cases[i] )
		printf(" %s->%d", m.first.c_str(), m.second->inst_num);
	printf("\n");
	}

const CompiledStmt ZAM::CompileInExpr(const NameExpr* n1,
				const NameExpr* n2, const ConstExpr* c2,
				const NameExpr* n3, const ConstExpr* c3)
	{
	auto op2 = n2 ? (Expr*) n2 : (Expr*) c2;
	auto op3 = n3 ? (Expr*) n3 : (Expr*) c3;

	ZOp a;

	if ( op2->Type()->Tag() == TYPE_PATTERN )
		a = n2 ? (n3 ? OP_P_IN_S_VVV : OP_P_IN_S_VVC) : OP_P_IN_S_VCV;

	else if ( op2->Type()->Tag() == TYPE_STRING )
		a = n2 ? (n3 ? OP_S_IN_S_VVV : OP_S_IN_S_VVC) : OP_S_IN_S_VCV;

	else if ( op2->Type()->Tag() == TYPE_ADDR &&
		  op3->Type()->Tag() == TYPE_SUBNET )
		a = n2 ? (n3 ? OP_A_IN_S_VVV : OP_A_IN_S_VVC) : OP_A_IN_S_VCV;

	else if ( op3->Type()->Tag() == TYPE_TABLE )
		a = n2 ? OP_VAL_IS_IN_TABLE_VVV : OP_CONST_IS_IN_TABLE_VCV;

	else
		reporter->InternalError("bad types when compiling \"in\"");

	auto s2 = n2 ? FrameSlot(n2) : 0;
	auto s3 = n3 ? FrameSlot(n3) : 0;
	auto s1 = Frame1Slot(n1, a);

	ZInst z;

	if ( n2 )
		{
		if ( n3 )
			z = ZInst(a, s1, s2, s3);
		else
			z = ZInst(a, s1, s2, c3);
		}
	else
		z = ZInst(a, s1, s3, c2);

	BroType* stmt_type =
		c2 ? c2->Type().get() : (c3 ? c3->Type().get() : nullptr);

	BroType* zt;

	if ( c2 )
		zt = c2->Type().get();
	else if ( c3 )
		zt = c3->Type().get();
	else
		zt = n2->Type().get();

	z.SetType(zt);

	return AddInst(z);
	}

const CompiledStmt ZAM::CompileInExpr(const NameExpr* n1, const ListExpr* l,
					const NameExpr* n2, const ConstExpr* c)
	{
	auto& l_e = l->Exprs();
	int n = l_e.length();

	// Look for a very common special case: l is a single-element list,
	// and n2 is present rather than c.  For these, we can save a lot
	// of cycles by not building out a val-vec and then transforming it
	// into a list-val.
	if ( n == 1 && n2 )
		{
		ZInst z;

		if ( l_e[0]->Tag() == EXPR_NAME )
			{
			auto l_e0_n = l_e[0]->AsNameExpr();
			z = GenInst(this, OP_VAL_IS_IN_TABLE_VVV, n1,
						l_e0_n, n2);
			}

		else
			{
			auto l_e0_c = l_e[0]->AsConstExpr();
			z = GenInst(this, OP_CONST_IS_IN_TABLE_VCV, n1, l_e0_c, n2);
			}

		z.t = l_e[0]->Type().release();
		return AddInst(z);
		}

	// Also somewhat common is a 2-element index.  Here, one or both of
	// the elements might be a constant, which makes things messier.

	if ( n == 2 && n2 &&
	     (l_e[0]->Tag() == EXPR_NAME || l_e[1]->Tag() == EXPR_NAME) )
		{
		auto is_name0 = l_e[0]->Tag() == EXPR_NAME;
		auto is_name1 = l_e[1]->Tag() == EXPR_NAME;

		auto l_e0_n = is_name0 ? l_e[0]->AsNameExpr() : nullptr;
		auto l_e1_n = is_name1 ? l_e[1]->AsNameExpr() : nullptr;

		auto l_e0_c = is_name0 ? nullptr : l_e[0]->AsConstExpr();
		auto l_e1_c = is_name1 ? nullptr : l_e[1]->AsConstExpr();

		ZInst z;

		if ( l_e0_n && l_e1_n )
			{
			z = GenInst(this, OP_VAL2_IS_IN_TABLE_VVVV,
					n1, l_e0_n, l_e1_n, n2);
			z.t = l_e0_n->Type().release();
			}

		else if ( l_e0_n )
			{
			z = GenInst(this, OP_VAL2_IS_IN_TABLE_VVVC,
					n1, l_e0_n, n2, l_e1_c);
			z.t = l_e0_n->Type().release();
			}

		else if ( l_e1_n )
			{
			z = GenInst(this, OP_VAL2_IS_IN_TABLE_VVCV,
					n1, l_e1_n, n2, l_e0_c);
			z.t = l_e1_n->Type().release();
			}

		else
			{
			// Ugh, both are constants.  Assign first to
			// a temporary.
			auto slot = NewSlot(l_e0_c->Type());
			auto z = ZInst(OP_ASSIGN_CONST_VC, slot, l_e0_c);
			z.CheckIfManaged(l_e0_c);
			(void) AddInst(z);

			z = ZInst(OP_VAL2_IS_IN_TABLE_VVVC, FrameSlot(n1),
					slot, FrameSlot(n2), l_e1_c);
			z.op_type = OP_VVVC;
			z.t = l_e0_c->Type().release();
			}

		return AddInst(z);
		}

	auto aggr = n2 ? (Expr*) n2 : (Expr*) c;

	ZOp op;

	if ( aggr->Type()->Tag() == TYPE_VECTOR )
		op = n2 ? OP_INDEX_IS_IN_VECTOR_VV : OP_INDEX_IS_IN_VECTOR_VC;
	else
		op = n2 ? OP_LIST_IS_IN_TABLE_VV : OP_LIST_IS_IN_TABLE_VC;

	ZInst z;

	if ( n2 )
		z = ZInst(op, Frame1Slot(n1, op), FrameSlot(n2));
	else
		z = ZInst(op, Frame1Slot(n1, op), c);

	z.aux = InternalBuildVals(l);

	return AddInst(z);
	}

const CompiledStmt ZAM::CompileIndex(const NameExpr* n1, const NameExpr* n2,
					const ListExpr* l)
	{
	ZInst z;

	int n = l->Exprs().length();
	auto n2t = n2->Type();
	auto n2tag = n2t->Tag();

	if ( n == 1 )
		{
		auto ind = l->Exprs()[0];
		auto var_ind = ind->Tag() == EXPR_NAME;
		auto n3 = var_ind ? ind->AsNameExpr() : nullptr;
		auto c3 = var_ind ? nullptr : ind->AsConstExpr();
		bro_uint_t c = 0;

		int n2_slot = FrameSlot(n2);

		if ( ! var_ind )
			{
			if ( ind->Type()->Tag() == TYPE_COUNT )
				c = c3->Value()->AsCount();
			else if ( ind->Type()->Tag() == TYPE_INT )
				c = c3->Value()->AsInt();
			}

		if ( n2tag == TYPE_STRING )
			{
			if ( n3 )
				{
				int n3_slot = FrameSlot(n3);
				auto zop = OP_INDEX_STRING_VVV;
				z = ZInst(zop, Frame1Slot(n1, zop),
						n2_slot, n3_slot);
				}
			else
				{
				auto zop = OP_INDEX_STRINGC_VVV;
				z = ZInst(zop, Frame1Slot(n1, zop), n2_slot, c);
				z.op_type = OP_VVV_I3;
				}

			return AddInst(z);
			}

		if ( n2tag == TYPE_VECTOR )
			{
			if ( n3 )
				{
				int n3_slot = FrameSlot(n3);
				auto zop = OP_INDEX_VEC_VVV;
				z = ZInst(zop, Frame1Slot(n1, zop),
						n2_slot, n3_slot);
				}
			else
				{
				auto zop = OP_INDEX_VECC_VVV;
				z = ZInst(zop, Frame1Slot(n1, zop), n2_slot, c);
				z.op_type = OP_VVV_I3;
				}

			z.SetType(n1->Type());
			z.e = n2;
			return AddInst(z);
			}

		if ( n2tag == TYPE_TABLE )
			{
			if ( n3 )
				{
				int n3_slot = FrameSlot(n3);
				auto zop = AssignmentFlavor(OP_TABLE_INDEX1_VVV,
							n1->Type()->Tag());
				z = ZInst(zop, Frame1Slot(n1, zop), n2_slot,
						n3_slot);
				z.SetType(n3->Type());
				}

			else
				{
				auto zop = AssignmentFlavor(OP_TABLE_INDEX1_VVC,
							n1->Type()->Tag());
				z = ZInst(zop, Frame1Slot(n1, zop),
							n2_slot, c3);
				}

			return AddInst(z);
			}
		}

	auto indexes = l->Exprs();
	int n2_slot = FrameSlot(n2);

	ZOp op;

	switch ( n2tag ) {
	case TYPE_VECTOR:
		op = n == 1 ? OP_INDEX_VEC_VV : OP_INDEX_VEC_SLICE_VV;
		z = ZInst(op, Frame1Slot(n1, op), n2_slot);
		z.SetType(n2->Type());
		break;

	case TYPE_TABLE:
		op = OP_TABLE_INDEX_VV;
		z = ZInst(op, Frame1Slot(n1, op), n2_slot);
		z.SetType(n1->Type());
		break;

	case TYPE_STRING:
		op = OP_INDEX_STRING_SLICE_VV;
		z = ZInst(op, Frame1Slot(n1, op), n2_slot);
		z.SetType(n1->Type());
		break;

	default:
		reporter->InternalError("bad aggregate type when compiling index");
	}

	z.aux = InternalBuildVals(l);
	z.CheckIfManaged(n1);

	return AddInst(z);
	}

const CompiledStmt ZAM::CompileSchedule(const NameExpr* n, const ConstExpr* c,
					int is_interval, EventHandler* h,
					const ListExpr* l)
	{
	int len = l->Exprs().length();
	ZInst z;

	if ( len == 0 )
		{
		z = n ? ZInst(OP_SCHEDULE0_ViH, FrameSlot(n), is_interval) :
			ZInst(OP_SCHEDULE0_CiH, is_interval, c);
		z.op_type = n ? OP_VV_I2 : OP_VC_I1;
		}

	else
		{
		if ( n )
			{
			z = ZInst(OP_SCHEDULE_ViHL, FrameSlot(n), is_interval);
			z.op_type = OP_VV_I2;
			}
		else
			{
			z = ZInst(OP_SCHEDULE_CiHL, is_interval, c);
			z.op_type = OP_VC_I1;
			}

		z.aux = InternalBuildVals(l);
		}

	z.event_handler = h;

	return AddInst(z);
	}

const CompiledStmt ZAM::CompileEvent(EventHandler* h, const ListExpr* l)
	{
	ZInst z(OP_EVENT_HL);
	z.aux = InternalBuildVals(l);
	z.event_handler = h;

	return AddInst(z);
	}

void ZAM::SyncGlobals(const BroObj* o)
	{
	SyncGlobals(pf->globals, o);
	}

void ZAM::SyncGlobals(std::unordered_set<ID*>& globals, const BroObj* o)
	{
	auto mgr = reducer->GetDefSetsMgr();
	auto entry_rds = mgr->GetPreMaxRDs(body);

	auto curr_rds = o ?
		mgr->GetPreMaxRDs(o) : mgr->GetPostMaxRDs(LastStmt());

	bool could_be_dirty = false;

	for ( auto g : globals )
		{
		auto g_di = mgr->GetConstID_DI(g);
		auto entry_dps = entry_rds->GetDefPoints(g_di);
		auto curr_dps = curr_rds->GetDefPoints(g_di);

		if ( ! entry_rds->SameDefPoints(entry_dps, curr_dps) )
			{
			modified_globals.insert(g);
			could_be_dirty = true;
			}
		}

	if ( could_be_dirty )
		(void) AddInst(ZInst(OP_SYNC_GLOBALS_X));
	}

const CompiledStmt  ZAM::AssignedToGlobal(const ID* global_id)
	{
	// We used to need this before adding ZAMOp1Flavor.  We keep
	// it as a structure since it potentially could be needed
	// in the future.
	return EmptyStmt();
	}

void ZAM::PushGoTos(GoToSets& gotos)
	{
	vector<CompiledStmt> vi;
	gotos.push_back(vi);
	}

void ZAM::ResolveGoTos(GoToSets& gotos, const InstLabel l)
	{
	auto& g = gotos.back();

	for ( int i = 0; i < g.size(); ++i )
		SetGoTo(g[i], l);

	gotos.pop_back();
	}

CompiledStmt ZAM::GenGoTo(GoToSet& v)
	{
	auto g = GoToStub();
	v.push_back(g.stmt_num);

	return g;
	}

CompiledStmt ZAM::GoToStub()
	{
	ZInst z(OP_GOTO_V, 0);
	z.op_type = OP_V_I1;
	return AddInst(z);
	}

CompiledStmt ZAM::GoTo(const InstLabel l)
	{
	ZInst inst(OP_GOTO_V, 0);
	inst.target = l;
	inst.target_slot = 1;
	inst.op_type = OP_V_I1;
	return AddInst(inst);
	}

InstLabel ZAM::GoToTarget(const CompiledStmt s)
	{
	return insts1[s.stmt_num];
	}

InstLabel ZAM::GoToTargetBeyond(const CompiledStmt s)
	{
	int n = s.stmt_num;

	if ( n == insts1.size() - 1 )
		{
		if ( ! pending_inst )
			pending_inst = new ZInst();

		return pending_inst;
		}

	return insts1[n+1];
	}

CompiledStmt ZAM::PrevStmt(const CompiledStmt s)
	{
	return CompiledStmt(s.stmt_num - 1);
	}

void ZAM::SetTarget(ZInst* inst, const InstLabel l, int slot)
	{
	if ( inst->target )
		{
		ASSERT(! inst->target2);
		inst->target2 = l;
		inst->target2_slot = slot;
		}
	else
		{
		inst->target = l;
		inst->target_slot = slot;
		}
	}

void ZAM::SetV1(CompiledStmt s, const InstLabel l)
	{
	auto inst = insts1[s.stmt_num];
	SetTarget(inst, l, 1);
	ASSERT(inst->op_type == OP_V || inst->op_type == OP_V_I1);
	inst->op_type = OP_V_I1;
	}

void ZAM::SetV2(CompiledStmt s, const InstLabel l)
	{
	auto inst = insts1[s.stmt_num];
	SetTarget(inst, l, 2);

	if ( inst->op_type == OP_VV )
		inst->op_type = OP_VV_I2;

	else if ( inst->op_type == OP_VVC )
		inst->op_type = OP_VVC_I2;

	else
		ASSERT(inst->op_type == OP_VV_I2 || inst->op_type == OP_VVC_I2);
	}

void ZAM::SetV3(CompiledStmt s, const InstLabel l)
	{
	auto inst = insts1[s.stmt_num];
	SetTarget(inst, l, 3);

	ASSERT(inst->op_type == OP_VVV || inst->op_type == OP_VVV_I3 ||
		inst->op_type == OP_VVV_I2_I3);
	if ( inst->op_type != OP_VVV_I2_I3 )
		inst->op_type = OP_VVV_I3;
	}

void ZAM::SetV4(CompiledStmt s, const InstLabel l)
	{
	auto inst = insts1[s.stmt_num];
	SetTarget(inst, l, 4);

	ASSERT(inst->op_type == OP_VVVV || inst->op_type == OP_VVVV_I4);
	if ( inst->op_type != OP_VVVV_I4 )
		inst->op_type = OP_VVVV_I4;
	}


int ZAM::FrameSlot(const ID* id)
	{
	auto slot = RawSlot(id);

	if ( id->IsGlobal() )
		(void) LoadGlobal(frame_denizens[slot]);

	return slot;
	}

int ZAM::Frame1Slot(const ID* id, ZAMOp1Flavor fl)
	{
	auto slot = RawSlot(id);

	switch ( fl ) {
	case OP1_READ:
		if ( id->IsGlobal() )
			(void) LoadGlobal(frame_denizens[slot]);
		break;

	case OP1_WRITE:
		if ( id->IsGlobal() )
			mark_dirty = global_id_to_info[id];
		break;

        case OP1_READ_WRITE:
		if ( id->IsGlobal() )
			{
			(void) LoadGlobal(frame_denizens[slot]);
			mark_dirty = global_id_to_info[id];
			}
		break;

	case OP1_INTERNAL:
		break;
	}

	return slot;
	}

int ZAM::RawSlot(const ID* id)
	{
	auto id_slot = frame_layout1.find(id);

	if ( id_slot == frame_layout1.end() )
		reporter->InternalError("ID %s missing from frame layout", id->Name());

	return id_slot->second;
	}

bool ZAM::HasFrameSlot(const ID* id) const
	{
	return frame_layout1.find(id) != frame_layout1.end();
	}

int ZAM::NewSlot(bool is_managed)
	{
	char buf[8192];
	snprintf(buf, sizeof buf, "#internal-%d#", frame_size);

	// In the following, all that matters is that for managed
	// types we pick a tag that will be viewed as managed, and
	// vice versa.
	auto tag = is_managed ? TYPE_TABLE : TYPE_VOID;

	auto internal_reg = new ID(buf, SCOPE_FUNCTION, false);
	internal_reg->SetType(base_type(tag));

	return AddToFrame(internal_reg);
	}

bool ZAM::CheckAnyType(const BroType* any_type, const BroType* expected_type,
			const Stmt* associated_stmt) const
	{
	if ( IsAny(expected_type) )
		return true;

	if ( ! same_type(any_type, expected_type, false, false) )
		{
		auto at = any_type->Tag();
		auto et = expected_type->Tag();

		if ( at == TYPE_RECORD && et == TYPE_RECORD )
			{
			auto at_r = any_type->AsRecordType();
			auto et_r = expected_type->AsRecordType();

			if ( record_promotion_compatible(et_r, at_r) )
				return true;
			}

		char buf[8192];
		snprintf(buf, sizeof buf, "run-time type clash (%s/%s)",
			type_name(at), type_name(et));

		reporter->Error(buf, associated_stmt);
		return false;
		}

	return true;
	}

IntrusivePtr<Val> ResumptionAM::Exec(Frame* f, stmt_flow_type& flow) const
	{
	return am->DoExec(f, xfer_pc, flow);
	}

void ResumptionAM::StmtDescribe(ODesc* d) const
	{
	d->Add("resumption of compiled code");
	}

TraversalCode ResumptionAM::Traverse(TraversalCallback* cb) const
	{
	TraversalCode tc = cb->PreStmt(this);
	HANDLE_TC_STMT_PRE(tc);

	tc = cb->PostStmt(this);
	HANDLE_TC_STMT_POST(tc);
	}

// Unary vector operation of v1 <vec-op> v2.
static void vec_exec(ZOp op, VectorVal*& v1, VectorVal* v2)
	{
	// We could speed this up further still by gen'ing up an
	// instance of the loop inside each switch case (in which
	// case we might as well move the whole kit-and-caboodle
	// into the Exec method).  But that seems like a lot of
	// code bloat for only a very modest gain.

	auto& vec2 = v2->RawVector()->ConstVec();
	bool needs_management;

	if ( ! v1 )
		{
		auto vt = v2->Type()->AsVectorType();
		::Ref(vt);
		v1 = new VectorVal(vt);
		}

	v1->RawVector()->Resize(vec2.size());

	auto& vec1 = v1->RawVector()->ModVec();

	for ( unsigned int i = 0; i < vec2.size(); ++i )
		switch ( op ) {

#include "ZAM-Vec1EvalDefs.h"

		default:
			reporter->InternalError("bad invocation of VecExec");
		}
	}

// Binary vector operation of v1 = v2 <vec-op> v3.
static void vec_exec(ZOp op, BroType* yt, VectorVal*& v1,
			VectorVal* v2, const VectorVal* v3)
	{
	// See comment above re further speed-up.

	auto& vec2 = v2->RawVector()->ConstVec();
	auto& vec3 = v3->RawVector()->ConstVec();

	BroType* needs_management = v1 ? yt : nullptr;

	if ( ! v1 )
		{
		auto vt = v2->Type()->AsVectorType();
		::Ref(vt);
		v1 = new VectorVal(vt);
		}

	// ### This leaks if it's a vector-of-string becoming smaller.
	v1->RawVector()->Resize(vec2.size());

	auto& vec1 = v1->RawVector()->ModVec();

	for ( unsigned int i = 0; i < vec2.size(); ++i )
		switch ( op ) {

#include "ZAM-Vec2EvalDefs.h"

		default:
			reporter->InternalError("bad invocation of VecExec");
		}
	}