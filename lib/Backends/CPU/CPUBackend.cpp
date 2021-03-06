/**
 * Copyright (c) Glow Contributors. See CONTRIBUTORS file.
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

#include "CPUBackend.h"
#include "CPUFunction.h"
#include "CPULLVMIRGen.h"

#include "glow/Backend/BackendUtils.h"
#include "glow/Graph/Graph.h"
#include "glow/IR/Instrs.h"
#include "glow/LLVMIRCodeGen/LLVMIRGen.h"
#include "glow/Support/Debug.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"

using namespace glow;

/// We compile the standard library (libjit) to LLVM bitcode, and then convert
/// that binary data to an include file using an external utility (include-bin).
/// The resulting file is included here to compile the bitcode image into our
/// library.
static const unsigned char libjit_bc[] = {
#include "glow/CPU/libjit_bc.inc"
};
static const size_t libjit_bc_size = sizeof(libjit_bc);

bool CPUBackend::isOpSupported(const NodeInfo &NI) const {
  // Note: For brevity below, "X ==> Y, Z" signifes that Node X is IRGen'd into
  // Instructions Y and Z.
  switch (NI.getKind()) {
  case Kinded::Kind::BatchedReduceMinNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
        {ElemKind::FloatTy, ElemKind::Int32ITy, ElemKind::Int64ITy});

  case Kinded::Kind::AddNodeKind:
  case Kinded::Kind::MulNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
        {ElemKind::FloatTy, ElemKind::Int8QTy, ElemKind::Int32ITy,
         ElemKind::Int64ITy});

  case Kinded::Kind::SubNodeKind:
  case Kinded::Kind::MaxNodeKind:
  case Kinded::Kind::MinNodeKind:
  case Kinded::Kind::CPUMaxSplatNodeKind:
  case Kinded::Kind::BatchedReduceAddNodeKind:
  case Kinded::Kind::MatMulNodeKind:
  case Kinded::Kind::AvgPoolNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
        {ElemKind::FloatTy, ElemKind::Int8QTy});

  case Kinded::Kind::AdaptiveAvgPoolNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::FloatTy});

  case Kinded::Kind::MaxPoolNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy, ElemKind::Int8QTy}, {},
               {MaxPoolNode::ArgmaxIdx}) &&
           (NI.getOutElemTy(MaxPoolNode::ArgmaxIdx) == ElemKind::Int64ITy ||
            NI.getOutElemTy(MaxPoolNode::ArgmaxIdx) == ElemKind::Int32ITy);

  case Kinded::Kind::ArgMaxNodeKind:
  case Kinded::Kind::ArgMinNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy, ElemKind::Int8QTy}, {},
               {ArgMaxNode::ResultIdx}) &&
           (NI.getOutElemTy(ArgMaxNode::ResultIdx) == ElemKind::Int64ITy ||
            NI.getOutElemTy(ArgMaxNode::ResultIdx) == ElemKind::Int32ITy);

  case Kinded::Kind::ResizeNearestNodeKind:
  case Kinded::Kind::ResizeBilinearNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
        {ElemKind::FloatTy, ElemKind::Int8QTy, ElemKind::Int32QTy,
         ElemKind::Int32ITy, ElemKind::Int64ITy});

  case Kinded::Kind::SaveNodeKind:
  case Kinded::Kind::ReshapeNodeKind:
    // These are implemented via a Copy Instruction.
    return NI.allInputsAndOutputsHaveSameElemKind(
        {ElemKind::FloatTy, ElemKind::Int8QTy, ElemKind::Int32QTy,
         ElemKind::Int32ITy, ElemKind::Int64ITy, ElemKind::BoolTy});

    // InsertTensor ==> Copy + InsertTensor. Copy supports everything
    // ReshapeNode above supports, so InsertTensor is the limiting factor.
  case Kinded::Kind::InsertTensorNodeKind:
    // Concat ==> Splat + Insert. Both only support the following.
  case Kinded::Kind::ConcatNodeKind:
  case Kinded::Kind::SplatNodeKind:
  case Kinded::Kind::TouchNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
        {ElemKind::FloatTy, ElemKind::Int8QTy, ElemKind::Int64ITy,
         ElemKind::Int32ITy, ElemKind::BoolTy});
  case Kinded::Kind::SliceNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
        {ElemKind::FloatTy, ElemKind::Int8QTy, ElemKind::Int32QTy,
         ElemKind::Int32ITy, ElemKind::Int64ITy});
  case Kinded::Kind::SpaceToDepthNodeKind:
  case Kinded::Kind::DivNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
        {ElemKind::FloatTy, ElemKind::Int8QTy, ElemKind::Int64ITy,
         ElemKind::Int32ITy});

  case Kinded::Kind::TransposeNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
        {ElemKind::FloatTy, ElemKind::Int8QTy, ElemKind::Int64ITy,
         ElemKind::BoolTy});

  case Kinded::Kind::FlipNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
        {ElemKind::FloatTy, ElemKind::Int8QTy, ElemKind::Int16QTy,
         ElemKind::Int32QTy, ElemKind::Int32ITy, ElemKind::Int64ITy,
         ElemKind::BoolTy});

  case Kinded::Kind::SparseLengthsSumNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy}, {SparseLengthsSumNode::IndicesIdx,
                                     SparseLengthsSumNode::LengthsIdx}) &&
           (NI.getInElemTy(SparseLengthsSumNode::IndicesIdx) ==
                ElemKind::Int64ITy ||
            NI.getInElemTy(SparseLengthsSumNode::IndicesIdx) ==
                ElemKind::Int32ITy) &&
           (NI.getInElemTy(SparseLengthsSumNode::LengthsIdx) ==
            ElemKind::Int32ITy);

  case Kinded::Kind::SparseLengthsWeightedSumNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy},
               {SparseLengthsWeightedSumNode::IndicesIdx,
                SparseLengthsWeightedSumNode::LengthsIdx}) &&
           (NI.getInElemTy(SparseLengthsWeightedSumNode::IndicesIdx) ==
                ElemKind::Int64ITy ||
            NI.getInElemTy(SparseLengthsWeightedSumNode::IndicesIdx) ==
                ElemKind::Int32ITy) &&
           (NI.getInElemTy(SparseLengthsWeightedSumNode::LengthsIdx) ==
            ElemKind::Int32ITy);

  case Kinded::Kind::EmbeddingBagNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy},
               {EmbeddingBagNode::IndicesIdx, EmbeddingBagNode::OffsetsIdx}) &&
           (NI.getInElemTy(EmbeddingBagNode::IndicesIdx) ==
            ElemKind::Int64ITy) &&
           (NI.getInElemTy(EmbeddingBagNode::OffsetsIdx) == ElemKind::Int64ITy);

  case Kinded::Kind::SparseLengthsWeightedSumGradNodeKind:
    // GradOfInputNamedIndicesIdx and GradOfInputNamedLengthsIdx do not need to
    // be checked because they are not used.
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy},
               {SparseLengthsWeightedSumGradNode::IndicesIdx,
                SparseLengthsWeightedSumGradNode::LengthsIdx},
               {SparseLengthsWeightedSumGradNode::GradOfInputNamedIndicesIdx,
                SparseLengthsWeightedSumGradNode::
                    GradOfInputNamedLengthsIdx}) &&
           (NI.getInElemTy(SparseLengthsWeightedSumGradNode::IndicesIdx) ==
                ElemKind::Int64ITy ||
            NI.getInElemTy(SparseLengthsWeightedSumGradNode::IndicesIdx) ==
                ElemKind::Int32ITy) &&
           (NI.getInElemTy(SparseLengthsWeightedSumGradNode::LengthsIdx) ==
            ElemKind::Int32ITy);

  case Kinded::Kind::RowwiseQuantizedSparseLengthsWeightedSumNodeKind:
    return (NI.getInElemTy(
                RowwiseQuantizedSparseLengthsWeightedSumNode::DataIdx) ==
            ElemKind::UInt8QTy) &&
           (NI.getInElemTy(
                RowwiseQuantizedSparseLengthsWeightedSumNode::ScalesIdx) ==
            ElemKind::FloatTy) &&
           (NI.getInElemTy(
                RowwiseQuantizedSparseLengthsWeightedSumNode::OffsetsIdx) ==
            ElemKind::FloatTy) &&
           (NI.getInElemTy(
                RowwiseQuantizedSparseLengthsWeightedSumNode::WeightsIdx) ==
            ElemKind::FloatTy) &&
           (NI.getInElemTy(
                RowwiseQuantizedSparseLengthsWeightedSumNode::IndicesIdx) ==
                ElemKind::Int64ITy ||
            NI.getInElemTy(
                RowwiseQuantizedSparseLengthsWeightedSumNode::IndicesIdx) ==
                ElemKind::Int32ITy) &&
           (NI.getInElemTy(
                RowwiseQuantizedSparseLengthsWeightedSumNode::LengthsIdx) ==
            ElemKind::Int32ITy) &&
           (NI.getOutElemTy(
                RowwiseQuantizedSparseLengthsWeightedSumNode::ResultIdx) ==
            ElemKind::FloatTy);

  case Kinded::Kind::LengthsRangeFillNodeKind:
  case Kinded::Kind::LengthsToRangesNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::Int32ITy});

  case Kinded::Kind::IntLookupTableNodeKind:
  case Kinded::Kind::RescaleQuantizedNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::Int8QTy});

  case Kinded::Kind::PowNodeKind:
  case Kinded::Kind::AvgPoolGradNodeKind:
  case Kinded::Kind::QuantizationProfileNodeKind:
  case Kinded::Kind::CPUConvDKKC8NodeKind:
  case Kinded::Kind::LocalResponseNormalizationNodeKind:
  case Kinded::Kind::LocalResponseNormalizationGradNodeKind:
  case Kinded::Kind::LogNodeKind:
  case Kinded::Kind::TanhNodeKind:
  case Kinded::Kind::SigmoidNodeKind:
  case Kinded::Kind::ExpNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::FloatTy});

  case Kinded::Kind::ModuloNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
        {ElemKind::Int32ITy, ElemKind::Int64ITy});

  case Kinded::Kind::MaxPoolGradNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy},
               {MaxPoolGradNode::OriginalOutputForArgmaxIdx,
                MaxPoolGradNode::GradOfOriginalOutputNamedArgmaxIdx}) &&
           (NI.getInElemTy(MaxPoolGradNode::OriginalOutputForArgmaxIdx) ==
                ElemKind::Int64ITy ||
            NI.getInElemTy(MaxPoolGradNode::OriginalOutputForArgmaxIdx) ==
                ElemKind::Int32ITy) &&
           (NI.getInElemTy(
                MaxPoolGradNode::GradOfOriginalOutputNamedArgmaxIdx) ==
                ElemKind::Int64ITy ||
            NI.getInElemTy(
                MaxPoolGradNode::GradOfOriginalOutputNamedArgmaxIdx) ==
                ElemKind::Int32ITy);

  case Kinded::Kind::ConvolutionNodeKind:
    if (!NI.getInTy(ConvolutionNode::InputIdx)->isQuantizedType()) {
      return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::FloatTy});
    }

    return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::Int8QTy},
                                                  {ConvolutionNode::BiasIdx}) &&
           (NI.getInElemTy(ConvolutionNode::BiasIdx) == ElemKind::Int8QTy ||
            NI.getInElemTy(ConvolutionNode::BiasIdx) == ElemKind::Int32QTy);

  case Kinded::Kind::ChannelwiseQuantizedConvolutionNodeKind:
    return (NI.getInElemTy(ChannelwiseQuantizedConvolutionNode::InputIdx) ==
            ElemKind::Int8QTy) &&
           (NI.getInElemTy(ChannelwiseQuantizedConvolutionNode::FilterIdx) ==
            ElemKind::Int8QTy) &&
           ((NI.getInElemTy(ChannelwiseQuantizedConvolutionNode::BiasIdx) ==
             ElemKind::Int8QTy) ||
            (NI.getInElemTy(ChannelwiseQuantizedConvolutionNode::BiasIdx) ==
             ElemKind::Int32QTy)) &&
           (NI.getInElemTy(
                ChannelwiseQuantizedConvolutionNode::FilterScalesIdx) ==
            ElemKind::FloatTy) &&
           (NI.getInElemTy(
                ChannelwiseQuantizedConvolutionNode::FilterOffsetsIdx) ==
            ElemKind::Int32ITy) &&
           (NI.getInElemTy(
                ChannelwiseQuantizedConvolutionNode::BiasScalesIdx) ==
            ElemKind::FloatTy) &&
           (NI.getInElemTy(
                ChannelwiseQuantizedConvolutionNode::BiasOffsetsIdx) ==
            ElemKind::Int32ITy) &&
           (NI.getOutElemTy(ChannelwiseQuantizedConvolutionNode::ResultIdx) ==
            ElemKind::Int8QTy);

  case Kinded::Kind::ConvTransposeNodeKind:
    // TODO - not quantized support yet in libjit.
    return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::FloatTy});

  case Kinded::Kind::BatchedAddNodeKind:
    if (!NI.getInTy(BatchedAddNode::BatchIdx)->isQuantizedType()) {
      return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::FloatTy});
    }
    // Allow for Int8QTy or Int32QTy for the Slice input.
    return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::Int8QTy},
                                                  {BatchedAddNode::SliceIdx}) &&
           ((NI.getInElemTy(BatchedAddNode::SliceIdx) == ElemKind::Int8QTy) ||
            (NI.getInElemTy(BatchedAddNode::SliceIdx) == ElemKind::Int32QTy));

  case Kinded::Kind::GatherNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy, ElemKind::Int8QTy, ElemKind::Int64ITy,
                ElemKind::Int32ITy},
               {GatherNode::IndicesIdx}) &&
           ((NI.getInElemTy(GatherNode::IndicesIdx) == ElemKind::Int32ITy) ||
            (NI.getInElemTy(GatherNode::IndicesIdx) == ElemKind::Int64ITy));

  case Kinded::Kind::GatherRangesNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy, ElemKind::Int8QTy, ElemKind::Int64ITy,
                ElemKind::Int32ITy},
               {GatherRangesNode::RangesIdx}, {GatherRangesNode::LengthsIdx}) &&
           ((NI.getInElemTy(GatherRangesNode::RangesIdx) ==
             NI.getOutElemTy(GatherRangesNode::LengthsIdx)) &&
            ((NI.getOutElemTy(GatherRangesNode::LengthsIdx) ==
              ElemKind::Int32ITy) ||
             (NI.getOutElemTy(GatherRangesNode::LengthsIdx) ==
              ElemKind::Int64ITy)));

  case Kinded::Kind::ScatterDataNodeKind:
    // ScatterData ==> Copy + ScatterData. Copy supports everything
    // ReshapeNode above supports, however ScatterData only supports the
    // following.
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy, ElemKind::Int8QTy},
               {ScatterDataNode::IndicesIdx}) &&
           (NI.getInElemTy(ScatterDataNode::IndicesIdx) == ElemKind::Int64ITy ||
            NI.getInElemTy(ScatterDataNode::IndicesIdx) == ElemKind::Int32ITy);

  case Kinded::Kind::SelectNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy, ElemKind::Int8QTy, ElemKind::Int32ITy},
               {SelectNode::CondIdx}) &&
           ((NI.getInElemTy(SelectNode::CondIdx) == ElemKind::BoolTy));

  case Kinded::Kind::NotNodeKind:
  case Kinded::Kind::AndNodeKind:
  case Kinded::Kind::OrNodeKind:
  case Kinded::Kind::XorNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::BoolTy});

  case Kinded::Kind::AbsNodeKind:
  case Kinded::Kind::NegNodeKind:
  case Kinded::Kind::FloorNodeKind:
  case Kinded::Kind::CeilNodeKind:
  case Kinded::Kind::RoundNodeKind:
  case Kinded::Kind::SqrtNodeKind:
  case Kinded::Kind::RsqrtNodeKind:
  case Kinded::Kind::ReciprocalNodeKind:
  case Kinded::Kind::SinNodeKind:
  case Kinded::Kind::CosNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::FloatTy});

  case Kinded::Kind::CmpEQNodeKind:
  case Kinded::Kind::CmpNEQNodeKind:
  case Kinded::Kind::CmpLTNodeKind:
  case Kinded::Kind::CmpLTENodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy, ElemKind::Int8QTy, ElemKind::Int32ITy,
                ElemKind::Int64ITy},
               {}, {CmpEQNode::ResultIdx}) &&
           (NI.getOutElemTy(CmpEQNode::ResultIdx) == ElemKind::BoolTy);

  case Kinded::Kind::IsNaNNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::FloatTy}, {},
                                                  {IsNaNNode::ResultIdx}) &&
           (NI.getOutElemTy(IsNaNNode::ResultIdx) == ElemKind::BoolTy);

  case Kinded::Kind::TopKNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy, ElemKind::Int8QTy}, {},
               {TopKNode::IndicesIdx}) &&
           (NI.getOutElemTy(TopKNode::IndicesIdx) == ElemKind::Int64ITy ||
            NI.getOutElemTy(TopKNode::IndicesIdx) == ElemKind::Int32ITy);

  case Kinded::Kind::QuantizeNodeKind:
    return (NI.getInElemTy(QuantizeNode::InputIdx) == ElemKind::FloatTy) &&
           ((NI.getOutElemTy(QuantizeNode::ResultIdx) == ElemKind::Int8QTy) ||
            (NI.getOutElemTy(QuantizeNode::ResultIdx) == ElemKind::Int32QTy));

  case Kinded::Kind::DequantizeNodeKind:
    return (NI.getInElemTy(DequantizeNode::InputIdx) == ElemKind::Int8QTy) &&
           (NI.getOutElemTy(DequantizeNode::ResultIdx) == ElemKind::FloatTy);

  case Kinded::Kind::SoftMaxNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind({ElemKind::FloatTy},
                                                  {SoftMaxNode::SelectedIdx}) &&
           (NI.getInElemTy(SoftMaxNode::SelectedIdx) == ElemKind::Int64ITy ||
            NI.getInElemTy(SoftMaxNode::SelectedIdx) == ElemKind::Int32ITy);

  case Kinded::Kind::CrossEntropyLossNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy}, {CrossEntropyLossNode::LabelsIdx}) &&
           (NI.getInElemTy(CrossEntropyLossNode::LabelsIdx) ==
                ElemKind::Int64ITy ||
            NI.getInElemTy(CrossEntropyLossNode::LabelsIdx) ==
                ElemKind::Int32ITy);

  case Kinded::Kind::LengthsSumNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy}, {LengthsSumNode::LengthsIdx}) &&
           (NI.getInElemTy(LengthsSumNode::LengthsIdx) == ElemKind::Int32ITy);

  case Kinded::Kind::EmbeddingBagByteRowwiseOffsetsNodeKind:
    return (NI.getInElemTy(EmbeddingBagByteRowwiseOffsetsNode::DataIdx) ==
            ElemKind::UInt8FusedQTy) &&
           (NI.getInElemTy(EmbeddingBagByteRowwiseOffsetsNode::WeightsIdx) ==
            ElemKind::FloatTy) &&
           (NI.getInElemTy(EmbeddingBagByteRowwiseOffsetsNode::IndicesIdx) ==
            ElemKind::Int64ITy) &&
           (NI.getInElemTy(EmbeddingBagByteRowwiseOffsetsNode::OffsetsIdx) ==
            ElemKind::Int64ITy) &&
           (NI.getOutElemTy(EmbeddingBagByteRowwiseOffsetsNode::ResultIdx) ==
            ElemKind::FloatTy);

  case Kinded::Kind::FusedRowwiseQuantizedSparseLengthsWeightedSumNodeKind:
    return (NI.getInElemTy(
                FusedRowwiseQuantizedSparseLengthsWeightedSumNode::DataIdx) ==
            ElemKind::UInt8FusedQTy) &&
           (NI.getInElemTy(FusedRowwiseQuantizedSparseLengthsWeightedSumNode::
                               WeightsIdx) == ElemKind::FloatTy) &&
           ((NI.getInElemTy(FusedRowwiseQuantizedSparseLengthsWeightedSumNode::
                                IndicesIdx) == ElemKind::Int64ITy ||
             NI.getInElemTy(FusedRowwiseQuantizedSparseLengthsWeightedSumNode::
                                IndicesIdx) == ElemKind::Int32ITy)) &&
           (NI.getInElemTy(FusedRowwiseQuantizedSparseLengthsWeightedSumNode::
                               LengthsIdx) == ElemKind::Int32ITy) &&
           (NI.getOutElemTy(
                FusedRowwiseQuantizedSparseLengthsWeightedSumNode::ResultIdx) ==
            ElemKind::FloatTy);

  case Kinded::Kind::RowwiseQuantizedFullyConnectedNodeKind:
    return (NI.getInElemTy(RowwiseQuantizedFullyConnectedNode::InputIdx) ==
            ElemKind::Int8QTy) &&
           (NI.getInElemTy(RowwiseQuantizedFullyConnectedNode::WeightsIdx) ==
            ElemKind::Int8QTy) &&
           (NI.getInElemTy(RowwiseQuantizedFullyConnectedNode::ScalesIdx) ==
            ElemKind::FloatTy) &&
           (NI.getInElemTy(RowwiseQuantizedFullyConnectedNode::OffsetsIdx) ==
            ElemKind::Int32ITy) &&
           (NI.getInElemTy(RowwiseQuantizedFullyConnectedNode::BiasIdx) ==
                ElemKind::Int8QTy ||
            NI.getInElemTy(RowwiseQuantizedFullyConnectedNode::BiasIdx) ==
                ElemKind::Int32QTy) &&
           (NI.getOutElemTy(RowwiseQuantizedFullyConnectedNode::ResultIdx) ==
            ElemKind::Int8QTy);

  case Kinded::Kind::SparseToDenseNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy}, {SparseToDenseNode::IndicesIdx}) &&
           (NI.getInElemTy(SparseToDenseNode::IndicesIdx) ==
                ElemKind::Int64ITy ||
            NI.getInElemTy(SparseToDenseNode::IndicesIdx) ==
                ElemKind::Int32ITy);

  case Kinded::Kind::SoftMaxGradNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy}, {SoftMaxGradNode::SelectedIdx},
               {SoftMaxGradNode::GradOfInputNamedSelectedIdx}) &&
           (NI.getInElemTy(SoftMaxGradNode::SelectedIdx) ==
                ElemKind::Int64ITy ||
            NI.getInElemTy(SoftMaxGradNode::SelectedIdx) == ElemKind::Int32ITy);

  case Kinded::Kind::ConvolutionGradNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
        {ElemKind::FloatTy}, {},
        {ConvolutionGradNode::GradOfInputNamedInputIdx});

  case Kinded::Kind::CrossEntropyLossGradNodeKind:
    return NI.allInputsAndOutputsHaveSameElemKind(
               {ElemKind::FloatTy}, {CrossEntropyLossGradNode::LabelsIdx},
               {CrossEntropyLossGradNode::GradOfInputNamedLabelsIdx}) &&
           (NI.getInElemTy(CrossEntropyLossGradNode::LabelsIdx) ==
            ElemKind::Int64ITy) &&
           (NI.getOutElemTy(
                CrossEntropyLossGradNode::GradOfInputNamedLabelsIdx) ==
            ElemKind::Int64ITy);

  case Kinded::Kind::TraceEventNodeKind:
    return NI.getInElemTy(TraceEventNode::DataIdx) == ElemKind::Int64ITy;

  case Kinded::Kind::NonMaxSuppressionNodeKind:
    return NI.getInElemTy(NonMaxSuppressionNode::BoxesIdx) ==
               ElemKind::FloatTy &&
           NI.getInElemTy(NonMaxSuppressionNode::ScoresIdx) ==
               ElemKind::FloatTy &&
           (NI.getOutElemTy(NonMaxSuppressionNode::IndicesIdx) ==
                ElemKind::Int32ITy ||
            NI.getOutElemTy(NonMaxSuppressionNode::IndicesIdx) ==
                ElemKind::Int64ITy) &&
           (NI.getOutElemTy(
                NonMaxSuppressionNode::NumberOfSelectedIndicesIdx) ==
                ElemKind::Int32ITy ||
            NI.getOutElemTy(
                NonMaxSuppressionNode::NumberOfSelectedIndicesIdx) ==
                ElemKind::Int64ITy);

  case Kinded::Kind::AudioSpectrogramNodeKind:
    return NI.getInElemTy(AudioSpectrogramNode::InputIdx) ==
               ElemKind::FloatTy &&
           NI.getOutElemTy(AudioSpectrogramNode::SpectrogramIdx) ==
               ElemKind::FloatTy;

  case Kinded::Kind::MFCCNodeKind:
    return NI.getInElemTy(MFCCNode::SpectrogramIdx) == ElemKind::FloatTy &&
           NI.getOutElemTy(MFCCNode::CoefficientsIdx) == ElemKind::FloatTy;

  case Kinded::Kind::ConvertToNodeKind:
    return ((NI.getInElemTy(ConvertToNode::InputIdx) == ElemKind::Int32ITy) &&
            (NI.getOutElemTy(ConvertToNode::ResultIdx) == ElemKind::FloatTy)) ||
           ((NI.getInElemTy(ConvertToNode::InputIdx) == ElemKind::BoolTy) &&
            (NI.getOutElemTy(ConvertToNode::ResultIdx) == ElemKind::FloatTy)) ||
           ((NI.getInElemTy(ConvertToNode::InputIdx) == ElemKind::Int64ITy) &&
            (NI.getOutElemTy(ConvertToNode::ResultIdx) ==
             ElemKind::Int32ITy)) ||
           ((NI.getInElemTy(ConvertToNode::InputIdx) == ElemKind::Int32ITy) &&
            (NI.getOutElemTy(ConvertToNode::ResultIdx) == ElemKind::Int64ITy));

  default:
    return false;
  }
}

bool CPUBackend::shouldLower(const Node *N) const {
  switch (N->getKind()) {
  case Kinded::Kind::ConvolutionNodeKind:
  case Kinded::Kind::SparseLengthsSumNodeKind:
    return false;
  default:
    return true;
  }
}

unsigned CPUBackend::numDevices() {
  return std::thread::hardware_concurrency();
}

std::unique_ptr<CompiledFunction> CPUBackend::createCompiledFunction(
    std::unique_ptr<llvm::orc::GlowJIT> JIT,
    runtime::RuntimeBundle &&runtimeBundle) const {
  return glow::make_unique<CPUFunction>(std::move(JIT),
                                        std::move(runtimeBundle));
}

std::unique_ptr<LLVMIRGen>
CPUBackend::createIRGen(const IRFunction *IR,
                        AllocationsInfo &allocationsInfo) const {
  CPULLVMIRGen *irgen =
      new CPULLVMIRGen(IR, allocationsInfo, "", getLibjitBitcode());
  return std::unique_ptr<CPULLVMIRGen>(irgen);
}

llvm::StringRef CPUBackend::getLibjitBitcode() const {
  return llvm::StringRef(reinterpret_cast<const char *>(libjit_bc),
                         libjit_bc_size);
}

/// \returns true if network supports Type Lowering from \p T1 to \p T2.
/// Populates PrecisionConfiguration with black list of operations that can't be
/// converted.
bool CPUBackend::canDoIndexTypeDemotion(
    ElemKind fromTy, ElemKind toTy, PrecisionConfiguration &precConfig) const {
  precConfig.precisionModeKindSet.insert(Kinded::Kind::EmbeddingBagNodeKind);
  precConfig.precisionModeKindSet.insert(
      Kinded::Kind::EmbeddingBagByteRowwiseOffsetsNodeKind);
  precConfig.precisionModeKindSet.insert(
      Kinded::Kind::FusedRowwiseQuantizedSparseLengthsSumNodeKind);
  precConfig.precisionModeKindSet.insert(
      Kinded::Kind::FusedRowwiseQuantizedSparseLengthsWeightedSumNodeKind);
  precConfig.precisionModeKindSet.insert(
      Kinded::Kind::SparseToDenseMaskNodeKind);
  return fromTy == ElemKind::Int64ITy && toTy == ElemKind::Int32ITy;
}
