// clang-format off
/*
 * SPDX-FileCopyrightText: Copyright (c) 2023-present NVIDIA CORPORATION & AFFILIATES.
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
// clang-format on
#include <ir_utils.h>
#include <kernel_ir.h>
#include <lower2device.h>

#include <lower_double_buffer.h>

#include <algorithm>
#include <iterator>
#include <vector>

namespace nvfuser {

unsigned int getDoubleBufferAxisPosition(const TensorView* tv) {
  // Double-buffering prefetches the next subregion of the tensor by
  // doubling the allocation. The subregion is defined by the axes
  // at the CA position till the inner-most position. There must be
  // at least one axis that is outside (left) of the CA position,
  // which defines the loop where prefetching is applied. Therefore,
  // the CA position must be larger than 0.

  TORCH_INTERNAL_ASSERT(tv->getComputeAtPosition() > 0, tv->toString());

  // Unroll must not exist outside of double-buffer axis
  auto first_unroll_it = std::find_if(
      tv->domain()->domain().begin(),
      tv->domain()->domain().end(),
      [](const auto axis) {
        return axis->getParallelType() == ParallelType::Unroll;
      });

  const int first_unroll_pos =
      (int)std::distance(tv->domain()->domain().begin(), first_unroll_it);

  const int unroll_or_ca_pos =
      std::min((int)tv->getComputeAtPosition(), first_unroll_pos);

  TORCH_INTERNAL_ASSERT(
      unroll_or_ca_pos > 0,
      "Invalid tensor to double-buffer. Valid double buffer axis not found due to Unroll. ",
      tv->toString());

  int valid_pos = -1;
  // Skip parallelized or broadcast axes
  for (int i = unroll_or_ca_pos - 1; i >= 0; --i) {
    auto pt = tv->axis(i)->getParallelType();
    if (!isParallelTypeThread(pt) && !tv->axis(i)->isBroadcast()) {
      valid_pos = i;
      break;
    }
  }

  TORCH_INTERNAL_ASSERT(
      valid_pos >= 0,
      "Invalid tensor to double-buffer. Valid double buffer axis not found. ",
      tv->toString());

  return valid_pos;
}

IterDomain* getDoubleBufferAxis(const TensorView* tv) {
  return tv->axis((int)getDoubleBufferAxisPosition(tv));
}

void validateDoubleBufferedTensor(const TensorView* tv) {
  auto double_buffer_pos = getDoubleBufferAxisPosition(tv);

  // Like vectorization, only LoadStoreOp with another TensorView is
  // considered.
  auto def = tv->definition();
  TORCH_INTERNAL_ASSERT(
      def->isA<LoadStoreOp>(),
      "Invalid tensor to double-buffer. Only tensor defined by LoadStoreOp is supported: ",
      def->toString());

  TORCH_INTERNAL_ASSERT(
      def->input(0)->isA<TensorView>(),
      "Invalid tensor to double-buffer. Only tensor defined by LoadStoreOp with TensorView is supported: ",
      def->toString());

  TORCH_INTERNAL_ASSERT(
      !tv->hasComputeWith(),
      "computeWith is not supported with double buffering: ",
      tv->toString());

  // Require the producer tensor to have been computed entirely for
  // the double-buffering loop. Otherwise, the producer itself would
  // also need to be double-bufferred.
  auto producer = def->input(0)->as<TensorView>();
  TORCH_INTERNAL_ASSERT(
      producer->getComputePosition(tv) <= double_buffer_pos,
      "Invalid tensor to double-buffer. The computeAt position of the producer tensor must be moved left: ",
      producer->toString());

  // Not strictly necessary, but only gmem -> smem or local and smem -> local
  // are allowed.
  const auto p_mem_type = producer->getMemoryType();
  const auto c_mem_type = tv->getMemoryType();
  TORCH_INTERNAL_ASSERT(
      (p_mem_type == MemoryType::Global &&
       (c_mem_type == MemoryType::Shared || c_mem_type == MemoryType::Local)) ||
          (c_mem_type == MemoryType::Local),
      "Invalid tensor to double-buffer: ",
      tv->toString(),
      ". Producer memory type: ",
      p_mem_type,
      ". Consumer memory type: ",
      c_mem_type);

  return;
}

namespace {

// Initial inspection of a fusion to find and validate double buffered tensors
class DoubleBufferFusionInspector : private IterVisitor {
 public:
  DoubleBufferFusionInspector(Fusion* fusion, DoubleBufferInfo& db_info)
      : db_info_(db_info) {
    traverse(fusion);
  }

 private:
  using IterVisitor::handle;

  void handle(TensorView* tv) final {
    if (!(tv->isDoubleBuffered() || tv->isCircularBuffered())) {
      return;
    }

    TORCH_INTERNAL_ASSERT(
        tv->definition(), "Fusion input shouldn't be double buffered.", tv);

    validateDoubleBufferedTensor(tv);

    auto db_axis = getDoubleBufferAxis(tv);

    db_info_.setDoubleBufferAxis(tv, db_axis);
  }

 private:
  DoubleBufferInfo& db_info_;
};

// The epilogue loop is only created when the producer of a double
// buffer tensor is on smem, in which case it would otherwise require
// an additional predicate to guard buffer overruns. When it's on
// gmem, that isn't the case, so it does not need to create an
// epilogue loop.
bool requireEpilogue(const std::vector<Expr*>& exprs) {
  return std::any_of(exprs.begin(), exprs.end(), [](const Expr* expr) {
    return expr->input(0)->as<TensorView>()->getMemoryType() ==
        MemoryType::Shared;
  });
}

bool isGmemIncrement(Expr* expr) {
  if (auto loop = dynamic_cast<kir::ForLoop*>(expr)) {
    if (loop->body().exprs().size() != 1) {
      return false;
    }
    return isGmemIncrement(loop->body().exprs()[0]);
  } else if (auto address_compute = dynamic_cast<kir::AddressCompute*>(expr)) {
    return address_compute->opType() ==
        kir::AddressCompute::AddressComputeOpType::GMEM_INCREMENT;
  }
  return false;
}

//! Hoists the gmem increment ops to the beginning of the loop
//!  within the scope of the given loop.
//! Note: [Gmem Increment Hoisting]
//!
//! This optimization is very useful when inplace increment
//!  is used on the global memory pointers.
//! Before this optimization, the code would look like:
//!
//!  for i in ... // main loop
//!    load.global ... [ptr]
//!    // Here we actually have an anti-dependency (WAR) on
//!    //  the register holding ptr and could result in
//!    //  non-ideal performance when we do not have enough
//!    //  instructions to put between the load and the increment.
//!    // depending on how many other instructions we have
//!    //   within this loop.
//!    ptr += increment_value
//!
//! After this transformation, the code looks like:
//!  ptr -=increment_value // a naive way to compensate
//!                        //  for the first iter.
//!  for i in ... // main loop
//!    ptr += increment_value
//!    // This is actually ok as integer instructions
//!    //   are usually much faster than memory.
//!    load.global ... [ptr]
//!
//! This function hoists the pointer increments, in the given
//!  loop, assuming that the decrements have been inserted
//!  on the CircularInitProlog stage.
kir::ForLoop* hoistGmemIncrement(kir::ForLoop* fl) {
  auto hoisted_loop = IrBuilder::create<kir::ForLoop>(fl);

  // insert all gmem increment exprs
  for (auto expr : fl->body().exprs()) {
    if (isGmemIncrement(expr)) {
      hoisted_loop->body().push_back(expr);
    }
  }

  // insert all non gmem increment exprs
  for (auto expr : fl->body().exprs()) {
    if (!isGmemIncrement(expr)) {
      hoisted_loop->body().push_back(expr);
    }
  }

  return hoisted_loop;
}

// Replicates double buffer loops for Prologue, Main, and
// Epilogue. Prologue only copies the load expressions of double
// buffered tensors, whereas Epilogue does any expression other than
// the loads. Main copies everything.
class DoubleBufferLoopCloner : public kir::IrVisitor {
 public:
  static kir::ForLoop* clone(
      kir::ForLoop* double_buffer_loop,
      const std::vector<Expr*>& double_buffer_load_exprs,
      DoubleBufferLoopStage loop_type) {
    DoubleBufferLoopCloner cloner(
        double_buffer_loop, double_buffer_load_exprs, loop_type);
    cloner.clone();
    return cloner.cloned_top_level_loop_;
  }

 private:
  DoubleBufferLoopCloner(
      kir::ForLoop* double_buffer_loop,
      const std::vector<Expr*>& double_buffer_load_exprs,
      DoubleBufferLoopStage loop_type)
      : double_buffer_loop_(double_buffer_loop),
        double_buffer_load_exprs_(double_buffer_load_exprs),
        loop_type_(loop_type) {}

  using kir::IrVisitor::handle;

  void clone() {
    const auto gpu_lower = GpuLower::current();

    // Cloning the double buffer loop as follows:
    //
    // Prologue: 0 to 1
    // Main: 0 to (extent-1)
    // Epilogue: (extent-1) to extent

    auto index = GpuLower::current()->caMap()->getIndexVariable(
        double_buffer_loop_->iter_domain(), loop_type_);
    auto start = double_buffer_loop_->start();
    auto stop = double_buffer_loop_->stop();
    auto stage_depth = gpu_lower->doubleBufferInfo().getStageDepthFor(
        double_buffer_loop_->iter_domain());

    if (loop_type_ == DoubleBufferLoopStage::Prolog) {
      TORCH_INTERNAL_ASSERT(start->isZeroInt());
      stop = SimplifyingIrBuilder::create<Int>(stage_depth - 1);
    } else if (
        loop_type_ == DoubleBufferLoopStage::Main &&
        requireEpilogue(double_buffer_load_exprs_)) {
      stop = IrBuilder::subExpr(
          double_buffer_loop_->stop(), gpu_lower->kernel()->oneVal());
    } else if (loop_type_ == DoubleBufferLoopStage::Epilog) {
      TORCH_INTERNAL_ASSERT(requireEpilogue(double_buffer_load_exprs_));
      start = IrBuilder::subExpr(
          double_buffer_loop_->stop(),
          SimplifyingIrBuilder::create<Int>(stage_depth - 1));
    } else if (loop_type_ == DoubleBufferLoopStage::CircularInitProlog) {
      // See [Predicate Peeling Interaction with Circular Buffering]
      TORCH_INTERNAL_ASSERT(start->isZeroInt());
      start = SimplifyingIrBuilder::create<Int>(stage_depth - 1);
      stop = SimplifyingIrBuilder::create<Int>(stage_depth);
    }

    cloned_top_level_loop_ = IrBuilder::create<kir::ForLoop>(
        double_buffer_loop_->iter_domain(),
        index,
        start,
        stop,
        gpu_lower->kernel()->oneVal(),
        false,
        nullptr,
        double_buffer_loop_->isUnrollRequired(),
        double_buffer_loop_->loopTransformInfo().doubleBufferStage(loop_type_));

    handle(double_buffer_loop_);

    // insert double buffer switching for the read offset:
    if (loop_type_ == DoubleBufferLoopStage::Main) {
      auto& db_info = GpuLower::current()->doubleBufferInfo();

      for (auto load : double_buffer_load_exprs_) {
        if (auto tv_out = ir_utils::getTvOutput(load)) {
          // calculate the switching size
          auto switch_size = db_info.getOriginalAllocSize(tv_out);
          auto switch_size_in_byte = SimplifyingIrBuilder::mulExpr(
              switch_size,
              SimplifyingIrBuilder::create<Int>(dataTypeSize(tv_out->dtype())));

          // insert db switch expressions:
          // Note:[Uniform Double Buffer Offset]
          // This modification is to encourage usage of uniform registers on
          // sm75+ when
          //  accessing shared memory double buffered tensors.
          // The code before transformation:
          //   for i in ... // double buffer loop
          //     ... = ld.shared [... + (i%5) * double_buffer_size]
          // The above code doesn't explictly specify that the double buffer
          // switch
          //  component is uniform. The following transformed code makes it
          //  explicit:
          //   for i in ... // double buffer loop
          //     ... = ld.shared [... + switch_index]
          //     doubleBufferSwitch(switch_index);
          //  So that the double buffer indices are all placed in uniform reg.

          auto maybe_read_index = db_info.getReadSwitchIndex(tv_out);
          if (maybe_read_index.has_value()) {
            // Instantiate and insert the update operator.
            auto address_compute =
                SimplifyingIrBuilder::create<kir::AddressCompute>(
                    tv_out,
                    maybe_read_index.value(),
                    switch_size_in_byte,
                    0, // assume this path only supports read
                       // so offset is 0
                    db_info.getStageDepthFor(
                        double_buffer_loop_->iter_domain()));

            cloned_top_level_loop_->body().push_back(address_compute);
          }
        }
      }
    }

    // Hoist the address increment in the double buffer main
    // loop, see also [Gmem Increment Hoisting]
    if (loop_type_ == DoubleBufferLoopStage::Main &&
        std::any_of(
            double_buffer_loop_->body().exprs().begin(),
            double_buffer_loop_->body().exprs().end(),
            isGmemIncrement) &&
        // FIXME:
        // Below is current condition that is required for gmem increment
        //  hoisting because the gmem decrement is currently placed in
        //  CircularInitProlog which requires predicate peeling to
        //  be generated.
        // To fix this should probably dedicate another double buffer
        //  loop stage, maybe GmemPointerDecrement, that is reserved
        //  for placing the gmem decrement before the main loop stage.
        GpuLower::current()->predicatePeelingInfo().shouldPeelLoop(
            double_buffer_loop_)) {
      cloned_top_level_loop_ = hoistGmemIncrement(cloned_top_level_loop_);
    }
  }

  void handle(kir::ForLoop* fl) final {
    kir::ForLoop* cloned_loop = fl == double_buffer_loop_
        ? cloned_top_level_loop_
        : IrBuilder::create<kir::ForLoop>(fl);

    cloned_scopes_.push_back(&cloned_loop->body());

    kir::IrVisitor::handle(fl);

    cloned_scopes_.pop_back();

    // Add the cloned loop into the parent loop body only when the
    // cloned loop contains expressions.
    if (!cloned_loop->body().empty() && !cloned_scopes_.empty()) {
      cloned_scopes_.back()->push_back(cloned_loop);
    }
  }

  void handle(kir::IfThenElse* ite) final {
    TORCH_INTERNAL_ASSERT(false, "No IfThenElse should exist yet");
  }

  void handle(Expr* expr) final {
    if (expr->isA<kir::ForLoop>() || expr->isA<kir::IfThenElse>()) {
      kir::IrVisitor::handle(expr);
      return;
    }

    TORCH_INTERNAL_ASSERT(!cloned_scopes_.empty());

    if (loop_type_ == DoubleBufferLoopStage::Main) {
      if (!canOmitInitInMainLoop(expr, double_buffer_loop_)) {
        cloned_scopes_.back()->push_back(expr);
      }
      return;
    }

    // In Prologue and Epilogue, either load expressions or anything
    // else are copied. Note that there can be multiple exprs defining
    // double buffered TVs (e.g., buffer initialization).

    auto out_tv = ir_utils::getTvOutput(expr);
    const auto is_double_buffer_load_expr = std::any_of(
        double_buffer_load_exprs_.begin(),
        double_buffer_load_exprs_.end(),
        [out_tv](const auto load_expr) {
          auto double_buffer_tv = ir_utils::getTvOutput(load_expr);
          TORCH_INTERNAL_ASSERT(double_buffer_tv != nullptr);
          return out_tv == double_buffer_tv;
        });

    if ((loop_type_ == DoubleBufferLoopStage::Prolog &&
         is_double_buffer_load_expr) ||
        (loop_type_ == DoubleBufferLoopStage::Epilog &&
         !is_double_buffer_load_expr)) {
      if (lower_utils::supportInlinePredicate(expr) &&
          expr->isA<LoadStoreOp>()) {
        auto ldst = expr->as<LoadStoreOp>();
        cloned_scopes_.back()->push_back(IrBuilder::create<LoadStoreOp>(
            ldst->opType(), ldst->out(), ldst->in()));
      } else {
        cloned_scopes_.back()->push_back(expr);
      }
    } else if (
        loop_type_ == DoubleBufferLoopStage::CircularInitProlog &&
        is_double_buffer_load_expr) {
      // Only need the init expressions in circular init prolog stage
      if (ir_utils::isTensorScalarFillOp(expr)) {
        cloned_scopes_.back()->push_back(expr);
      }
    }

    if (loop_type_ == DoubleBufferLoopStage::CircularInitProlog) {
      // Convert the address compute ops to decrement in the circular
      //  buffer init prolog, see [Gmem Increment Hoisting].
      if (auto address_compute = dynamic_cast<kir::AddressCompute*>(expr)) {
        if (address_compute->opType() ==
            kir::AddressCompute::AddressComputeOpType::GMEM_INCREMENT) {
          cloned_scopes_.back()->push_back(
              IrBuilder::create<kir::AddressCompute>(
                  address_compute->addressTv(),
                  address_compute->dataTv(),
                  address_compute->incrementValue(),
                  true /* is_decrement */));
        }
      }
    }

    // Include the double buffer update expressions in prologs too as
    //  prolog does write into the double buffered space.
    if (loop_type_ == DoubleBufferLoopStage::Prolog) {
      if (auto address_compute = dynamic_cast<kir::AddressCompute*>(expr)) {
        if (address_compute->opType() ==
            kir::AddressCompute::AddressComputeOpType::DOUBLE_BUFFER_UPDATE) {
          if (std::any_of(
                  double_buffer_load_exprs_.begin(),
                  double_buffer_load_exprs_.end(),
                  [address_compute](Expr* expr) {
                    return ir_utils::getTvOutput(expr)->sameAs(
                        address_compute->dataTv());
                  })) {
            cloned_scopes_.back()->push_back(expr);
          }
        }
      }
    }

    if (loop_type_ != DoubleBufferLoopStage::CircularInitProlog) {
      if (auto address_compute = dynamic_cast<kir::AddressCompute*>(expr)) {
        if (address_compute->opType() ==
            kir::AddressCompute::AddressComputeOpType::GMEM_INCREMENT) {
          cloned_scopes_.back()->push_back(expr);
        }
      }
    }
  }

  //! Returns true if the expression is an initialization expr that
  //!  can be omitted in main loop.
  //! See [Predicate Peeling Interaction with Circular Buffering]
  bool canOmitInitInMainLoop(Expr* expr, kir::ForLoop* double_buffer_loop) {
    // Check that this is an initialization for cp.async.
    if (!ir_utils::isCpAsyncInit(expr) ||
        !GpuLower::current()->predicatePeelingInfo().shouldPeelLoop(
            double_buffer_loop)) {
      return false;
    }

    auto out_tv = ir_utils::getTvOutput(expr);

    // Check that the double buffer loop is the main stage of
    //  the loop defining out_tv as there might be multiple
    //  loops that realize double buffers.
    bool db_loop_found = false;
    const auto& ca_map = GpuLower::current()->caMap();

    if (!(out_tv->isDoubleBuffered() || out_tv->isCircularBuffered()) ||
        !ca_map->areMapped(
            GpuLower::current()->doubleBufferInfo().getDoubleBufferAxis(out_tv),
            double_buffer_loop->iter_domain(),
            IdMappingMode::LOOP)) {
      return false;
    }

    // This optimization only applies when all the loops on the
    //  inner side of the double buffer main loop are either
    //  constant unrolled or parallel.
    // TODO:
    //  Buffer alias and broadcast resolution might still
    // break this. These are not showing in matmul kernels but
    // would need to build out support for general safty usage.
    for (auto id : out_tv->domain()->domain()) {
      if (db_loop_found) {
        auto loop_concrete_id =
            ca_map->getConcreteMappedID(id, IdMappingMode::LOOP);

        if (!loop_concrete_id->isParallelized() &&
            !loop_concrete_id->extent()->isConstInt()) {
          return false;
        }
      }

      db_loop_found = db_loop_found ||
          ca_map->areMapped(
              id, double_buffer_loop->iter_domain(), IdMappingMode::LOOP);
    }

    // Only when double buffer loop was found on out_tv could useful
    //  information have been inferred by this function.
    return db_loop_found;
  }

 private:
  kir::ForLoop* double_buffer_loop_ = nullptr;
  const std::vector<Expr*>& double_buffer_load_exprs_;
  const DoubleBufferLoopStage loop_type_;

  kir::ForLoop* cloned_top_level_loop_ = nullptr;
  std::deque<kir::Scope*> cloned_scopes_;
};

using InsertionInfo = std::unordered_map<kir::ForLoop*, std::vector<Expr*>>;

class IsDoubleBufferLoadLoop : public kir::IrVisitor {
 public:
  static bool check(
      Expr* expr,
      const std::vector<Expr*>& double_buffer_load_exprs) {
    IsDoubleBufferLoadLoop checker(double_buffer_load_exprs);
    return checker.check(expr);
  }

 private:
  IsDoubleBufferLoadLoop(const std::vector<Expr*>& double_buffer_load_exprs)
      : double_buffer_load_exprs_(double_buffer_load_exprs) {}

  using kir::IrVisitor::handle;

  bool check(Expr* expr) {
    handle(expr);
    return result_;
  }

  void handle(Expr* expr) final {
    if (result_) {
      return;
    }
    if (std::find(
            double_buffer_load_exprs_.begin(),
            double_buffer_load_exprs_.end(),
            expr) != double_buffer_load_exprs_.end()) {
      result_ = true;
      return;
    }
    IrVisitor::handle(expr);
  }

 private:
  const std::vector<Expr*>& double_buffer_load_exprs_;
  bool result_ = false;
};

// Traverse lowered loop-nests and find all double buffer loops and
// associated load expressions.
class DoubleBufferLoopNestInspector : private kir::IrVisitor {
 public:
  static InsertionInfo run(const std::vector<Expr*>& exprs) {
    DoubleBufferLoopNestInspector inspector(exprs);
    return inspector.insertion_info_;
  }

 private:
  DoubleBufferLoopNestInspector(const std::vector<Expr*>& exprs) {
    handle(exprs);
  }

  using kir::IrVisitor::handle;

  // Collect double buffer related information on a expr
  //  that is a memory load, i.e. a LoadStore or a Set.
  void handlePossibleLoadExpr(Expr* expr) {
    const auto gpu_lower = GpuLower::current();

    auto out_tv = ir_utils::getTvOutput(expr);

    if (out_tv == nullptr) {
      return;
    }

    // Ignore init loop
    if (!(out_tv->isDoubleBuffered() || out_tv->isCircularBuffered()) ||
        !expr->input(0)->isA<TensorView>()) {
      return;
    }

    auto double_buffer_loop =
        gpu_lower->doubleBufferInfo().getDoubleBufferLoop(out_tv, for_loops_);

    TORCH_INTERNAL_ASSERT(
        double_buffer_loop != nullptr,
        "No double buffer loop found for a double buffered tensor: ",
        out_tv->toString());

    validateDoubleBufferLoop(double_buffer_loop);

    insertion_info_[double_buffer_loop].push_back(expr);
  }

  void handle(UnaryOp* uop) final {
    handlePossibleLoadExpr(uop);
  }

  void handle(LoadStoreOp* ldst) final {
    handlePossibleLoadExpr(ldst);
  }

  static void validateDoubleBufferLoop(kir::ForLoop* loop) {
    TORCH_INTERNAL_ASSERT(
        loop->start()->isZeroInt(), "Unsupported loop: ", loop->toString());
    TORCH_INTERNAL_ASSERT(
        loop->step()->isOneInt(), "Unsupported loop: ", loop->toString());
    TORCH_INTERNAL_ASSERT(
        !loop->vectorize(),
        "Vectorized loop should not be the allocation loop for double-buffered tensor: ",
        loop->toString());
    TORCH_INTERNAL_ASSERT(
        !loop->vectorize_shift(),
        "Vectorize shift loop should not be the allocation loop for double-buffered tensor: ",
        loop->toString());
  }

  InsertionInfo insertion_info_;
};

// Apply double buffering transformations
class DoubleBufferInserter : private kir::ExprMutator {
 public:
  // When there exist multiple double buffer loops, apply
  // transformations to inner-most loops first. A single ExprMutator
  // pass can only process one loop.
  static std::vector<Expr*> run(
      const std::vector<Expr*>& exprs,
      InsertionInfo insertion_info) {
    auto inserted_exprs = exprs;
    while (!insertion_info.empty()) {
      DoubleBufferInserter inserter(inserted_exprs, insertion_info);
      inserted_exprs = inserter.exprs_;
    }
    return inserted_exprs;
  }

 private:
  DoubleBufferInserter(
      const std::vector<Expr*>& exprs,
      InsertionInfo& insertion_info)
      : insertion_info_(insertion_info) {
    auto num_double_buffer_loops = insertion_info.size();
    traverseAndInsert(exprs);
    TORCH_INTERNAL_ASSERT(processed_loop_ != nullptr);
    TORCH_INTERNAL_ASSERT(insertion_info.size() == num_double_buffer_loops - 1);
  }

  using kir::ExprMutator::handle;

  void handle(kir::ForLoop* loop) final {
    kir::ExprMutator::handle(loop);

    // If another loop is already taken care of, no more loop should
    // be done in the same pass
    if (processed_loop_ != nullptr) {
      return;
    }

    auto it = insertion_info_.find(loop);
    if (it == insertion_info_.end()) {
      return;
    }

    insert(loop, it->second);
    processed_loop_ = loop;
    insertion_info_.erase(loop);
  }

  void insert(
      kir::ForLoop* double_buffer_loop,
      const std::vector<Expr*>& loads) {
    // Allocate read switching index if they need to be updated
    //  independently. see [Uniform Double Buffer Offset]
    for (auto load : loads) {
      if (auto load_output = dynamic_cast<TensorView*>(load->output(0))) {
        auto uses = load_output->fusion()->unordered_uses(load_output);
        if (load_output->getMemoryType() == MemoryType::Shared &&
            (load_output->isDoubleBuffered() ||
             load_output->isCircularBuffered()) &&
            load_output->shouldLiftReadAddress() &&
            // TODO: read switch index is only enabled for ldmatrix
            //  at the moment.
            // Would need to extend the ld.shared usage to directly
            //  take pointers to use this in other cases.
            std::all_of(uses.begin(), uses.end(), ir_utils::isLdMatrixOp)) {
          auto switch_val = IrBuilder::create<Int>();
          switch_val->to32b();

          // Record the read switch indexing variable so it can be
          //  used in the indexing pass.
          // TODO: maybe want to do this in id graph instead
          GpuLower::current()->doubleBufferInfo().setReadSwitchIndex(
              load_output, switch_val);

          // Place allocation for the switching variable before the
          //  double buffer loop.
          auto index_alloc = IrBuilder::create<kir::Allocate>(
              switch_val,
              MemoryType::Local,
              GpuLower::current()->kernel()->oneVal(),
              true);
          registerInsertBefore(double_buffer_loop, index_alloc);
        }
      }
    }

    auto prologue_loop = DoubleBufferLoopCloner::clone(
        double_buffer_loop, loads, DoubleBufferLoopStage::Prolog);
    registerInsertBefore(double_buffer_loop, prologue_loop);

    auto write_to_smem =
        std::any_of(loads.begin(), loads.end(), [](const Expr* expr) {
          return expr->output(0)->as<TensorView>()->getMemoryType() ==
              MemoryType::Shared;
        });

    // If the double buffer loop is to be peeled. Will need to insert
    //  a circular buffer init stage to initialize the final stage of
    //  circular buffer space.
    if (GpuLower::current()->predicatePeelingInfo().shouldPeelLoop(
            double_buffer_loop) &&
        write_to_smem) {
      auto circular_init_loop = DoubleBufferLoopCloner::clone(
          double_buffer_loop, loads, DoubleBufferLoopStage::CircularInitProlog);
      registerInsertBefore(double_buffer_loop, circular_init_loop);
    }

    // RAW sync is not inserted for double buffered tensors. The only
    // exception is the prologue load.
    bool has_cpasync = false;
    if (write_to_smem) {
      // Here the initial sync before entering double buffer loop is
      //  inserted.

      // If any of the double buffered tensor in this double buffer
      //  loop is async copy. We want to wait for the gmem loads to
      //  finish before synchronizing the block.
      if (std::any_of(loads.begin(), loads.end(), ir_utils::isCpAsyncOp)) {
        auto stage_depth =
            GpuLower::current()->doubleBufferInfo().getStageDepthFor(
                double_buffer_loop->iter_domain());
        auto cp_async_wait =
            IrBuilder::create<kir::CpAsyncWait>(stage_depth - 2);
        prologue_loop->body().push_back(
            IrBuilder::create<kir::CpAsyncCommit>());
        registerInsertBefore(double_buffer_loop, cp_async_wait);
        has_cpasync = true;
      }

      // Insert the initial block sync before entering main loop.
      if (std::any_of(loads.begin(), loads.end(), [](Expr* expr) {
            return GpuLower::current()
                ->syncMap()
                ->needsRawSync(ir_utils::getTvOutput(expr))
                .hasTID();
          })) {
        // If any of the double buffered loads require sync, as indicated
        //  by sync info map, insert the sync before entering the double buffer
        //  loop.
        // TODO:
        //  Currently not supporting double buffer in gmem, but short to mid
        //  term not yet a priority to go for this case.
        auto sync = IrBuilder::create<kir::BlockSync>(false);
        registerInsertBefore(double_buffer_loop, sync);
      }
    }

    auto main_loop = DoubleBufferLoopCloner::clone(
        double_buffer_loop, loads, DoubleBufferLoopStage::Main);

    registerReplace(double_buffer_loop, main_loop);

    // Insert the wait instruction in this pass instead
    //  of relying on WAR sync pass to do it.
    // The WAR sync pass today would insert the wait function
    //  exactly where we need it but the purpose of this wait
    //  insertion isn't exactly WAR protection.
    //
    // TODO: [Double Buffer Sync]
    //  We might eventually want to move the block sync inserted
    //   by WAR pass here as well since this sync insertion is kind
    //   of both WAR and RAW (or neither RAW nor WAR, depends
    //   on how we look at it).
    // Eg. in the case when a intermediate
    //   tensor is double buffered.
    //
    //  __block_sync();    // This is the initial sync
    //  For i in ...       // Double buffer loop
    //     A[i%2] = ...;
    //     ...  = A[1-i%2];
    //     __block_sync();  // sync within loop
    //     ...
    //  The "sync within loop" can be placed anywhere in the
    //   double buffer loop while in the case of RAW and WAR
    //   there'd be extra insertion point restrictions.
    //  We are currently not actively exploring opportunities
    //   with this property of "double buffer sync" so this
    //   is more conceptual at the moment, aka low priority.
    if (has_cpasync) {
      insertCpAsyncCommitWaitInMainLoop(main_loop, loads);
    }

    if (requireEpilogue(loads)) {
      auto epilogue_loop = DoubleBufferLoopCloner::clone(
          double_buffer_loop, loads, DoubleBufferLoopStage::Epilog);
      registerInsertAfter(double_buffer_loop, epilogue_loop);
    }
  }

  // Simple conservative rule for inserting async copy wait
  //  primitive in the double buffer loop:
  void insertCpAsyncCommitWaitInMainLoop(
      kir::ForLoop* main_loop,
      const std::vector<Expr*>& loads) {
    TORCH_INTERNAL_ASSERT(
        !main_loop->body().empty(),
        "Double buffer sync insertion: empty main loop.");
    auto& exprs = main_loop->body().exprs();
    // Note: This pass explicitly assumes that WAR sync has been
    //  inserted so would need to be updated if we re-order the
    //  passes. Cleanups suggested in [Double Buffer Sync]
    //  would resolve this dependency on pass ordering.
    auto stage_depth = GpuLower::current()->doubleBufferInfo().getStageDepthFor(
        main_loop->iter_domain());
    auto cp_async_commit = IrBuilder::create<kir::CpAsyncCommit>();
    auto cp_async_wait = IrBuilder::create<kir::CpAsyncWait>(stage_depth - 2);

    // Find the last double buffer load in the main loop, and insert
    // cp.async.commit after it.
    std::vector<Expr*>::const_iterator last_double_buffer_load = exprs.end();
    for (auto it = exprs.begin(); it != exprs.end(); ++it) {
      if (IsDoubleBufferLoadLoop::check(*it, loads)) {
        last_double_buffer_load = it;
      }
    }
    TORCH_INTERNAL_ASSERT(last_double_buffer_load != exprs.end());
    std::vector<Expr*>::const_iterator commit_it =
        main_loop->body().insert(last_double_buffer_load + 1, cp_async_commit);

    // Check if a sync has been inserted by WAR sync pass.
    auto rend = std::make_reverse_iterator(commit_it);
    auto block_sync_it =
        std::find_if(exprs.rbegin(), rend, [](const Expr* expr) {
          return expr->isA<kir::BlockSync>();
        });
    if (block_sync_it == rend) {
      // If there's no sync, i.e. no tensor needs cross thread communication. We
      // still need a wait but it can just be anywhere after the cp.async.commit
      // in the loop. Chose to place at the end arbitrarily.
      main_loop->body().insert_after(exprs.back(), cp_async_wait);
    } else {
      // If a sync has been inserted, wait needs to be placed before the sync.
      main_loop->body().insert_before(*block_sync_it, cp_async_wait);
    }
  }

 private:
  InsertionInfo& insertion_info_;
  kir::ForLoop* processed_loop_ = nullptr;
};

} // namespace

void DoubleBufferInfo::build(Fusion* fusion) {
  DoubleBufferFusionInspector inspector(fusion, *this);

  // Build double buffered loop id's
  for (auto& info : map_) {
    auto double_buffer_axis = info.second.double_buffer_axis;
    // Keeps track of which loop disjoint set has been
    //  double buffered. In index allocation, one index
    //  variable would need to be allocated in each
    //  double buffer stage.
    concrete_double_buffered_loop_id_.insert(
        GpuLower::current()->caMap()->getConcreteMappedID(
            double_buffer_axis, IdMappingMode::LOOP));
  }
}

bool DoubleBufferInfo::isDoubleBufferedIterDomain(IterDomain* id) {
  auto concrete_loop_id = GpuLower::current()->caMap()->getConcreteMappedID(
      id, IdMappingMode::LOOP);
  return concrete_double_buffered_loop_id_.count(concrete_loop_id);
}

DoubleBufferInfo::TvInfo& DoubleBufferInfo::getTvInfo(const TensorView* tv) {
  TORCH_INTERNAL_ASSERT(
      tv->isDoubleBuffered() || tv->isCircularBuffered(),
      "Not a double-buffered tensor: ",
      tv->toString());
  return map_[tv];
}

void DoubleBufferInfo::setDoubleBufferAxis(
    const TensorView* tv,
    IterDomain* axis) {
  getTvInfo(tv).double_buffer_axis = axis;

  // Also validate the stage consistency with CA map.
  unsigned int stage_depth = 0;
  if (tv->isCircularBuffered()) {
    stage_depth = tv->circularBufferDepth();
  } else {
    // Double buffer is essentially
    //  circular buffer with depth 2.
    stage_depth = 2;
  }

  // Set and validate the new stage depth.
  setStageDepth(axis, stage_depth);
}

void DoubleBufferInfo::setStageDepth(IterDomain* id, unsigned int stage_depth) {
  auto concrete_loop_id = GpuLower::current()->caMap()->getConcreteMappedID(
      id, IdMappingMode::LOOP);

  auto maybe_exisiting_depth_it = stage_depth_.find(concrete_loop_id);
  if (maybe_exisiting_depth_it == stage_depth_.end()) {
    stage_depth_[concrete_loop_id] = stage_depth;
  } else {
    TORCH_INTERNAL_ASSERT(
        stage_depth == maybe_exisiting_depth_it->second,
        "Unsupported multiple depth pipelining, was set to ",
        maybe_exisiting_depth_it->second,
        " by ",
        maybe_exisiting_depth_it->first->toString(),
        " and then set to ",
        stage_depth,
        " by ",
        concrete_loop_id->toString());
  }
}

IterDomain* DoubleBufferInfo::getDoubleBufferAxis(const TensorView* tv) {
  if (!(tv->isDoubleBuffered() || tv->isCircularBuffered())) {
    return nullptr;
  }

  return getTvInfo(tv).double_buffer_axis;
}

unsigned int DoubleBufferInfo::getStageDepthFor(
    IterDomain* double_buffer_axis) {
  auto concrete_id = GpuLower::current()->caMap()->getConcreteMappedID(
      double_buffer_axis, IdMappingMode::LOOP);

  auto maybe_depth_it = stage_depth_.find(concrete_id);

  TORCH_INTERNAL_ASSERT(
      maybe_depth_it != stage_depth_.end(), "Stage depth not found");

  return maybe_depth_it->second;
}

kir::ForLoop* DoubleBufferInfo::getDoubleBufferLoop(
    IterDomain* axis,
    const std::vector<kir::ForLoop*>& loops,
    bool ignore_prologue) {
  auto loop_it = std::find_if(loops.begin(), loops.end(), [&](const auto loop) {
    return GpuLower::current()->caMap()->areMapped(
               loop->iter_domain(), axis, IdMappingMode::EXACT) &&
        (!ignore_prologue || !isProlog(loop->doubleBufferLoopStage()));
  });

  if (loop_it != loops.end()) {
    return *loop_it;
  } else {
    return nullptr;
  }
}

kir::ForLoop* DoubleBufferInfo::getDoubleBufferLoop(
    const TensorView* tv,
    const std::vector<kir::ForLoop*>& loops,
    bool ignore_prologue) {
  auto axis = getDoubleBufferAxis(tv);

  if (axis == nullptr) {
    return nullptr;
  }

  return getDoubleBufferLoop(axis, loops, ignore_prologue);
}

void DoubleBufferInfo::setOriginalAllocSize(
    const TensorView* tv,
    Val* original_alloc_size) {
  getTvInfo(tv).original_alloc_size = original_alloc_size;
}

Val* DoubleBufferInfo::getOriginalAllocSize(const TensorView* tv) {
  if (!(tv->isDoubleBuffered() || tv->isCircularBuffered())) {
    return nullptr;
  }

  return getTvInfo(tv).original_alloc_size;
}

std::vector<Expr*> DoubleBufferPass::run(const std::vector<Expr*>& exprs) {
  auto insertion_info = DoubleBufferLoopNestInspector::run(exprs);
  return DoubleBufferInserter::run(exprs, insertion_info);
}

} // namespace nvfuser
