//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#include "GetValue.h"
#include "CNTKLibrary.h"
#include "Variable.h"
#include "PrimitiveOpType.h"

#include <unordered_map>

#define _CRT_SECURE_NO_WARNINGS // "secure" CRT not available on all platforms  --add this at the top of all CPP files that give "function or variable may be unsafe" warnings

#define let const auto

using namespace std;

#define BreakPoint fprintf(stderr, "") // use this inside a conditional to be able to set a breakpoint in Release code

namespace CNTK
{
class Memoize
{
    // how graphs work in CNTK V2:
    //  - nodes := PrimitiveFunctions (incl. BlockFunction)
    //  - edges := Variables
    //  - net := CompositeFunction::m_allPrimitiveFunctions; duplicated for all refs to composites
    //  - output node: a node with additional ref to a net, created by calling Output() on a CompositeFunction
    // ownership:
    //  - nodes own edges: Functions hold shared_ptrs to m_inputs[] and m_outputs[]
    //  - edges do NOT own nodes
    //  - net owns full set of nodes
    //  - output node has a strong ref m_outputComposite to the CompositeFunction.
    //    This is injected when calling Output(), i.e. such an Output is really a different type w.r.t. ownership.

    // what we need to do:
    //  - operations that are computed in a batch:
    //     - Slice() ops (PrimitiveFunctions) batch the arguments
    //        - optimizes for the case that the arguments were already batched (they hold a m_lazySlice (pair<batchedOp, sliceIndex>))
    //     - a new PrimitiveFunction executes the batch immediately
    //     - the original operations get their m_value field filled with a slice into the batched op
    //        - this is done lazily; initially, they just remember a pair<batchedOp, sliceIndex>, as m_lazySlice
    //     - the batchedOp in the pair is also kept for batched backprop; it is a strong ref from Variable (cannot be in a cycle)
    //  - hence, we create N+1 new nodes:
    //     - the new batched op
    //     - Splice() for each of the N inputs
    // 'free' ops are always batched together and get executed first

    // predicate whether an op is only taking a view on its input
    // These are considered zero-cost, always batched whole-sale, and always done first.
    static bool IsViewOp(PrimitiveOpType op)
    {
        return
            op == PrimitiveOpType::StopGradient ||
            op == PrimitiveOpType::Pass         ||
            op == PrimitiveOpType::NoOp         ||
            op == PrimitiveOpType::BarrierOp    ||
            op == PrimitiveOpType::Reshape      ||
            op == PrimitiveOpType::Slice;
    }

    class NonOwningFunctionList // over Function, using m_link
    {
    protected:
        Function* head = nullptr;
        size_t count; // note: count is only in here for diagnostics; only needed in builder
    public:
        NonOwningFunctionList(Function* f) : head(f), count(1) { }
        Function* front() const { return head; }
        size_t size() const { return count; }
        class FunctionListIterator
        {
            Function* iter;
        public:
            FunctionListIterator(Function* f) : iter(f) { }
            Function* operator->() const { return iter; }
            Function* operator++() { iter = iter->m_link; return iter; }
            bool operator!=(const FunctionListIterator& other) { return iter != other.iter; }
        };
        FunctionListIterator begin() const { return front(); }
        FunctionListIterator end()   const { return nullptr; }
    };
    class NonOwningFunctionListBuilder : public NonOwningFunctionList // over Function, using m_link
    {
        Function* tail = nullptr;
    public:
        NonOwningFunctionListBuilder(Function* f) : NonOwningFunctionList(f), tail(f) { f->m_link = nullptr; }
        void append(Function* f)
        {
            if (!head)
                throw logic_error("NonOwningFunctionListBuilder must always be constructed with a first element");
            tail->m_link = f;
            tail = f;
            count++;
            f->m_link = nullptr;
        }
    };

    // class to manage the set of ready operations (the schedule)
    class ReadyOps
    {
        vector<NonOwningFunctionListBuilder> m_allOps; // m_allOps[] is a linked list
        // TODO: This must be turned into something hashable.
        // test whether two PrimitiveFunctions can be executed as a single batched operation
        static bool AreBatchable(const Function* a, const Function* b)
        {
            // first it must be the same operation
            let op = a->Op();
            // free ops always get batched; even if they have different op-codes
            if (IsViewOp(op) && op != PrimitiveOpType::BarrierOp)
                return true;
            // op codes must match
            if (op != b->Op())
                return false;
            // all input dimensions must match (with exception of a few special cases)
            assert(a->m_inputs.size() == b->m_inputs.size());
            for (size_t i = 0; i < a->m_inputs.size(); i++)
            {
                let& ia = a->m_inputs[i];
                let& ib = b->m_inputs[i];
                // there are a few special cases
                if (op == PrimitiveOpType::Times && i == 0)
                {
                    // for Times, the first arg must be the same object, not just the same shape
                    // TODO: a special case is a dot product, which we can write as ReduceSum(ElementTimes(a,b))
                    //       This would require to rewrite the graph though; can we do that?
                    if (ia.m_dataFields != ib.m_dataFields)
                        return false;
                }
                else
                {
                    // shapes must match
                    if (ia.Shape() != ib.Shape())
                        return false;
                }
                // another special case is reduction over all axes
            }
            // attributes must also match
            if (a->m_attributes != b->m_attributes)
                return false;
            // all match: we can batch
            return true;
        }
        // high-pri (free ops) are batched first; low-pri (Barrier) last.
        static int GetPriority(const Function* f)
        {
            let op = f->Op();
            if (op == PrimitiveOpType::BarrierOp) // Barrier goes last
                return 0;
            else if (IsViewOp(op)) // free ops go first
                return 2;
            else
                return 1;
        }
        // simple linked-list management
    public:
        // schedule an operation that has been confirmed ready
        void Schedule(Function* fp)
        {
            // this naive implementation just scans linearly
            // scan through all op sets to see if one is batchable with 'fp'
            for (auto iter = m_allOps.begin(); iter != m_allOps.end(); iter++)
            {
                if (AreBatchable(fp, iter->front()))
                {
                    iter->append(fp);
                    return;
                }
            }
            // none fit: open a new set
            m_allOps.push_back(NonOwningFunctionListBuilder(fp));
        }
        // test if no more ready ops
        bool empty() const { return m_allOps.empty(); }
        // select the next batched op to execute
        NonOwningFunctionList pop_best()
        {
            auto best = m_allOps.begin();
            // TODO: we could have 3 ready-ops sets, based on priority
            for (auto iter = best + 1; iter != m_allOps.end(); iter++)
            {
                let bestPri = GetPriority(best->front());
                let iterPri = GetPriority(iter->front());
                if (iterPri == -1)
                    BreakPoint;
                if (iterPri < bestPri)
                    continue;
                if (iterPri > bestPri ||
                    iter->size() > best->size())
                    best = iter;
            }
            // and remove this one from the list
            NonOwningFunctionList out = *best; // since NonOwningFunctionListBuilder uses unmanaged pointers, we can just copy it
            m_allOps.erase(best); // TODO: suboptimal complexity; but a list has the same problem
            return out;
        }
    };
    ReadyOps m_schedule;

    // recursively traverse the tree hanging off a Variable and
    //  - prepare all nodes for batched execution
    //  - schedule all ready operations
    // TODO: Once we are in the main build, change all Function to PrimitiveFunction directly.
    // TODO: What to do with multi-valued functions? Which ones are there? What is Combine(), a barrier?
    void TraverseFunctionTree(const Variable& v)
    {
        let& fields = *v.m_dataFields;
        if (fields.m_varKind == VariableKind::Input || fields.m_varKind == VariableKind::Placeholder)
            throw logic_error("Value() depends on Input or Placeholder, it is not knowable.");
        if (fields.m_varKind == VariableKind::Parameter || fields.m_varKind == VariableKind::Constant)
        {
            if (!fields.m_value) // force-initialize Parameters
                v.Value();
            if (!fields.m_value) // TODO: need to do this first
                throw logic_error("Parameter/Constant has no Value??");
            return;
        }
        auto& f = *fields.m_ownerFunction.lock();
        if (f.m_pendingInputs != -1) // already visited
            return;
        // determine how many inputs are pending
        // and also recurse
        size_t pendingInputs = 0;
        for (let& v : f.m_inputs)
        {
            let& fields = *v.m_dataFields;
            if (!fields.m_value)
            {
                TraverseFunctionTree(v);
                if (!fields.m_value) // (in case of a Parameter, we now may have a value)
                {
                    // no need for anything ref-counted since this is a local temp variable
                    let fi = v.m_dataFields->m_ownerFunction.lock();
                    if (!fi->m_notify1) // optimized for main case of 1 consumer. No std::vector in that case.
                        fi->m_notify1 = &f;
                    else
                        fi->m_notifyN.push_back(&f);
                    pendingInputs++;
                }
            }
        }
        f.m_pendingInputs = (int)pendingInputs;
        // if none then operation is ready
        if (pendingInputs == 0)
            m_schedule.Schedule(&f); // add to ready set
    }

    vector<Variable> m_inputs;
    vector<NDArrayViewPtr> m_args;
    size_t m_numBatches = 0;

    // batch-execute a set of ops that are known to be batchable
    void ExecuteBatchedOpAndUpdateSchedule(NonOwningFunctionList ops) // (note: NonOwningFunctionListBuilder is so small that it is best copied)
    {
        // TODO: need to handle ops that have >1 output, such as Combine(). Just don't batch them ever? Combine() is just a see-through anyway.
        // get a representative op
        let& f0 = *ops.front();
        let isFree = IsViewOp(f0.Op());
        if (!isFree)
            m_numBatches++;
        fprintf(stderr, "%d executing %d instances of %S -> %S\n", isFree ? -1 : (int)m_numBatches, (int)ops.size(), f0.OpName().c_str(), f0.m_outputs[0].Shape().AsString().c_str());
        m_inputs.resize(f0.m_inputs.size());
        m_args.resize(f0.m_inputs.size());
#if 1
        // for correctness testing of underlying mechanism, compute them without actual batching
        for (auto op = ops.begin(); op != ops.end(); ++op)
        //for (auto op : ops) // TODO: figure this out
        {
            if (op->m_outputs.size() != 1)
                throw logic_error("only functions with 1 output are supported");
            m_inputs.resize(op->m_inputs.size());
            m_args.resize(op->m_inputs.size());
            for (size_t i = 0; i < op->m_inputs.size(); i++)
            {
                m_args[i] = op->m_inputs[i].m_dataFields->m_value;
                if (!m_args[i])
                    throw logic_error("input unexpectedly not available");
            }
            NDArrayViewPtr out; // arena allocation will happen here
            op->m_outputs[0].m_dataFields->m_value =
                op->ComputeKnowableValue(op->Op(), m_args, op->Attributes(), op->m_outputs[0].Shape(), move(out));
        }
#else
        // batch all arguments
        // create a new PrimitiveFunction that executes this operation
        // compute its Value   --TODO: we need a lower-level executor that just takes the op, inputs, and attributes?
        // we can point to one of the ops for all info but the batched inputs
        // but then how do we bach-prop if we don't have an actual
        NDArrayViewPtr out;
        f0.ComputeKnowableValue(f0.Op(), args, f0.Attributes(), shape, move(out));
        // distribute the results to ops[]
#endif
        // update all ops' consumers
        for (auto op = ops.begin(); op != ops.end(); ++op)
        {
            // notify first consumer (this is a special optimization)
            auto* f = op->m_notify1;
            if (f)
            {
                if (f->m_pendingInputs <= 0)
                    throw logic_error("pending inputs already 0 yet we are executing it");
                f->m_pendingInputs--;
                // if it is now ready then schedule it
                if (f->m_pendingInputs == 0)
                    m_schedule.Schedule(f);
            }
            // notify all other consumer (this is a special optimization)
            for (auto* f : op->m_notifyN)
            {
                if (f->m_pendingInputs <= 0)
                    throw logic_error("pending inputs already 0 yet we are executing it");
                f->m_pendingInputs--;
                // if it is now ready then schedule it
                if (f->m_pendingInputs == 0)
                    m_schedule.Schedule(f);
            }
        }
    }

public:
    // Value(), computed with automatic batching
    NDArrayViewPtr operator()(const Variable& v)
    {
        // mark all nodes w.r.t. how many inputs they are waiting for before being computable
        let& fields = *v.m_dataFields;
        if (!fields.m_value)
        {
            // prepare and schedule first set
            TraverseFunctionTree(v);
            // compute the entire graph
            while (!m_schedule.empty())
            {
                // select the best amongst the scheduled ops
                auto opBatch = m_schedule.pop_best();
                // execute it, and also update all outputs' values and consumers, and the schedule 
                ExecuteBatchedOpAndUpdateSchedule(opBatch);
            }
        }
        return fields.m_value;
    }
}; // class
} // namespace

CNTK::NDArrayViewPtr GetValue(const CNTK::Variable& v)
{
#if 0
    // naive version
    return v.Value();
#else
    auto getValue = CNTK::Memoize(); // has some internal state
    return getValue(v);
#endif
}
