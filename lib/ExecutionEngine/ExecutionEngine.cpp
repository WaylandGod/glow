/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "glow/ExecutionEngine/ExecutionEngine.h"

#include "glow/Backends/Backend.h"
#include "glow/Graph/Graph.h"
#include "glow/IR/IR.h"
#include "glow/IR/IRBuilder.h"
#include "glow/IR/Instrs.h"
#include "glow/Optimizer/Optimizer.h"

using namespace glow;

ExecutionEngine::ExecutionEngine(BackendKind backendKind) {
  backendKind_ = backendKind;
  M_.reset(new Module());
  IR_.reset(new IRFunction());
  backend_.reset(createBackend(backendKind_, &*IR_));
}

// Set the code generator kind to \p backendKind.
void ExecutionEngine::setBackend(BackendKind backendKind) {
  backendKind_ = backendKind;
  backend_.reset(createBackend(backendKind, &*IR_));
}

void ExecutionEngine::reset() {
  if (IR_)
    IR_->clear();
  backend_.reset(createBackend(backendKind_, &*IR_));
}

ExecutionEngine::~ExecutionEngine() = default;

void ExecutionEngine::run(llvm::ArrayRef<Variable *> vars,
                          llvm::ArrayRef<Tensor *> inputs) {
  assert(inputs.size() == vars.size() &&
         "The number of inputs does not match the number of variables");
  assert(!IR_->getInstrs().empty() &&
         "Running a function with no instructions.");

  // Update the input variables.
  for (int i = 0, e = vars.size(); i < e; i++) {
    assert(vars[i]->getVisibilityKind() == VisibilityKind::Public &&
           "Trying to update a private variable");
    loadValueFromTensor(vars[i], inputs[i]);
  }

  backend_->doForwardPass();
}

void ExecutionEngine::runBatch(size_t iterations,
                               llvm::ArrayRef<Variable *> vars,
                               llvm::ArrayRef<Tensor *> inputs) {
  static size_t trainCounter = 0;

  assert(!inputs.empty() && "No inputs");
  assert(inputs.size() == vars.size() &&
         "The number of inputs does not match the number of variables");
  assert(!IR_->getInstrs().empty() &&
         "Running a function with no instructions.");

  // This is the size of one batch (the number of samples in the batch).
  size_t batchSize = vars[0]->getType()->dims()[0];

  for (size_t i = 0; i < iterations; i++) {
    // Pick up one slice from the input tensors, and load it into corresponding
    // network Variables. Then, run a single pass over the network.
    updateInputsAndRunNetwork(vars, inputs, trainCounter);

    trainCounter += batchSize;
  }
}

void ExecutionEngine::updateInputsAndRunNetwork(llvm::ArrayRef<Variable *> vars,
                                                llvm::ArrayRef<Tensor *> inputs,
                                                size_t sampleIdx) {
  // Update the input variables.
  for (int i = 0, e = vars.size(); i < e; i++) {
    loadValueFromTensorSlice(vars[i], inputs[i], sampleIdx);
  }

  // Run the network.
  backend_->doForwardPass();
}

void ExecutionEngine::loadValueFromTensorSlice(Variable *v, Tensor *input,
                                               size_t sampleIdx) {
  assert(v && "Invalid value");
  auto &t = v->getPayload();

  auto dim = input->dims();
  assert(t.dims().drop_front() == dim.drop_front() && "Invalid slice size");
  // Extract the n'th slice, that must be a tensor.
  size_t slc = sampleIdx % dim[0];
  t.copyConsecutiveSlices(input, slc);
}

void ExecutionEngine::loadValueFromTensor(Variable *v, Tensor *input) {
  assert(v && "Invalid value");
  auto &t = v->getPayload();
  auto dim = input->dims();
  (void)dim;
  assert(t.dims() == dim && "Invalid slice size");
  t.copyFrom(input);
}

void ExecutionEngine::generateIR(CompilationMode mode, Function *F) {
  // Reset the engine and start a new compilation process.
  reset();

  // Verify the function pre-optimization/lowering.
  F->verify();

  // Optimized the graph.
  ::glow::optimize(F, mode);

  // Allow the backend to transform the graph prior to lowering.
  if (backend_->transformPreLowering(F, mode)) {
    // Optimize the graph again after the backend transformation.
    // In particular, DCE is very likely to be useful.
    ::glow::optimize(F, mode);
  }

  // Lower the graph into a sequence of low-level linear algebra operations.
  ::glow::lower(F, mode, *backend_);

  // Optimized the graph again.
  ::glow::optimize(F, mode);

  // Allow the backend to transform the graph after lowering.
  if (backend_->transformPostLowering(F, mode)) {
    // Optimize the graph again after the backend transformation.
    // In particular, DCE is very likely to be useful.
    ::glow::optimize(F, mode);
  }

  /// Prepare the IR container to handle our function.
  IR_->setGraph(F);

  // Generate IR from the graph.
  IR_->generateIR();

  // Optimize the generated IR.
  ::glow::optimize(*IR_, mode, *backend_);
}

void ExecutionEngine::compile(CompilationMode mode, Function *F) {
  generateIR(mode, F);
  backend_->init();
}

void ExecutionEngine::save(CompilationMode mode, Function *F,
                           llvm::StringRef outputDir) {
  generateIR(mode, F);
  backend_->save(outputDir);
}
