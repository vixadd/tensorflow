/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/lite/kernels/internal/reference/quantize.h"

#include "CEVA_TFLM_lib.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/kernels/internal/quantization_util.h"
#include "tensorflow/lite/kernels/internal/reference/requantize.h"
#include "tensorflow/lite/kernels/internal/tensor_ctypes.h"
#include "tensorflow/lite/kernels/kernel_util.h"
#include "tensorflow/lite/micro/kernels/kernel_util.h"
#include "tensorflow/lite/micro/micro_utils.h"
#ifdef MCPS_MEASUREMENT
#include "mcps_macros.h"
#endif

namespace tflite {
namespace {

#if defined(CEVA_BX1) || defined(CEVA_SP500)
struct OpData {
  tflite::QuantizationParams quantization_params;
  // The scaling factor from input to output (aka the 'real multiplier') can
  // be represented as a fixed point multiplier plus a left shift.
  int32_t output_multiplier;
  int output_shift;

  int32_t input_zero_point;
};
#endif

void* Init(TfLiteContext* context, const char* buffer, size_t length) {
  TFLITE_DCHECK(context->AllocatePersistentBuffer != nullptr);
#if defined(CEVA_BX1) || defined(CEVA_SP500)
  return context->AllocatePersistentBuffer(context, sizeof(OpData));
#else
  return context->AllocatePersistentBuffer(context,
                                           sizeof(OpDataQuantizeReference));
#endif
}

TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node) {
  TFLITE_DCHECK(node->user_data != nullptr);
#if defined(CEVA_BX1) || defined(CEVA_SP500)
  OpData* data = static_cast<OpData*>(node->user_data);
#else
  auto* data = static_cast<OpDataQuantizeReference*>(node->user_data);
#endif

  TF_LITE_ENSURE_EQ(context, NumInputs(node), 1);
  TF_LITE_ENSURE_EQ(context, NumOutputs(node), 1);

  const TfLiteTensor* input = GetInput(context, node, 0);
  TfLiteTensor* output = GetOutput(context, node, 0);

  // TODO(b/128934713): Add support for fixed-point per-channel quantization.
  // Currently this only support affine per-layer quantization.
  TF_LITE_ENSURE_EQ(context, output->quantization.type,
                    kTfLiteAffineQuantization);
  const auto* affine_quantization =
      reinterpret_cast<TfLiteAffineQuantization*>(output->quantization.params);
  TF_LITE_ENSURE(context, affine_quantization);
  TF_LITE_ENSURE(context, affine_quantization->scale);
  TF_LITE_ENSURE(context, affine_quantization->scale->size == 1);

  TF_LITE_ENSURE(context, input->type == kTfLiteFloat32 ||
                              input->type == kTfLiteInt16 ||
                              input->type == kTfLiteInt8);
  TF_LITE_ENSURE(context, output->type == kTfLiteUInt8 ||
                              output->type == kTfLiteInt8 ||
                              output->type == kTfLiteInt16);

  if (((input->type == kTfLiteInt16 || input->type == kTfLiteInt8) &&
       output->type == kTfLiteInt8) ||
      (input->type == kTfLiteInt16 && output->type == kTfLiteInt16)) {
    double effective_scale = static_cast<double>(input->params.scale) /
                             static_cast<double>(output->params.scale);

    QuantizeMultiplier(effective_scale, &data->output_multiplier,
                       &data->output_shift);
  }

  data->quantization_params.zero_point = output->params.zero_point;
  data->quantization_params.scale = static_cast<double>(output->params.scale);

  data->input_zero_point = input->params.zero_point;
  return kTfLiteOk;
}

TfLiteStatus EvalCEVA(TfLiteContext* context, TfLiteNode* node) {
  TFLITE_DCHECK(node->user_data != nullptr);
  OpData* data = static_cast<OpData*>(node->user_data);

  const TfLiteEvalTensor* input = tflite::micro::GetEvalInput(context, node, 0);
  TfLiteEvalTensor* output = tflite::micro::GetEvalOutput(context, node, 0);

  if (input->type == kTfLiteFloat32) {
    switch (output->type) {
      case kTfLiteInt8: {
#ifdef ORIGINAL_IMPLEMENTATION
        reference_ops::AffineQuantize(
            data->quantization_params, tflite::micro::GetTensorShape(input),
            tflite::micro::GetTensorData<float>(input),
            tflite::micro::GetTensorShape(output),
            tflite::micro::GetTensorData<int8_t>(output));
#else  // ORIGINAL_IMPLEMENTATION
        const float* input_data = tflite::micro::GetTensorData<float>(input);
        int8_t* output_data = tflite::micro::GetTensorData<int8_t>(output);
        const int flat_size =
            MatchingFlatSize(tflite::micro::GetTensorShape(input),
                             tflite::micro::GetTensorShape(output));

#ifdef MCPS_MEASUREMENT
        MCPS_START_ONE;
#endif
        CEVA_TFLM_AffineQuantize_Int8(input_data, output_data, flat_size,
                                      data->quantization_params.scale,
                                      data->quantization_params.zero_point);
#ifdef MCPS_MEASUREMENT
        MCPS_STOP_ONE("Test params:CEVA_TFLM_AffineQuantize_Int8 loop = %d",
                      flat_size);
#endif

#endif  // ORIGINAL_IMPLEMENTATION
        break;
      }
      case kTfLiteUInt8:
        reference_ops::AffineQuantize(
            data->quantization_params, tflite::micro::GetTensorShape(input),
            tflite::micro::GetTensorData<float>(input),
            tflite::micro::GetTensorShape(output),
            tflite::micro::GetTensorData<uint8_t>(output));
        break;
      case kTfLiteInt16:
        reference_ops::AffineQuantize(
            data->quantization_params, tflite::micro::GetTensorShape(input),
            tflite::micro::GetTensorData<float>(input),
            tflite::micro::GetTensorShape(output),
            tflite::micro::GetTensorData<int16_t>(output));
        return kTfLiteOk;
      default:
        TF_LITE_KERNEL_LOG(context, "Input %s, output %s not supported.",
                           TfLiteTypeGetName(input->type),
                           TfLiteTypeGetName(output->type));
        return kTfLiteError;
    }
  } else if (input->type == kTfLiteInt16) {
    size_t size = ElementCount(*input->dims);
    switch (output->type) {
      case kTfLiteInt8:
        reference_ops::Requantize(tflite::micro::GetTensorData<int16_t>(input),
                                  size, data->output_multiplier,
                                  data->output_shift, data->input_zero_point,
                                  data->quantization_params.zero_point,
                                  tflite::micro::GetTensorData<int8_t>(output));
        break;
      case kTfLiteInt16:
        reference_ops::Requantize(
            tflite::micro::GetTensorData<int16_t>(input), size,
            data->output_multiplier, data->output_shift, data->input_zero_point,
            data->quantization_params.zero_point,
            tflite::micro::GetTensorData<int16_t>(output));
        return kTfLiteOk;
      default:
        TF_LITE_KERNEL_LOG(context, "Input %s, output %s not supported.",
                           TfLiteTypeGetName(input->type),
                           TfLiteTypeGetName(output->type));
        return kTfLiteError;
    }
  } else if (input->type == kTfLiteInt8) {
    // Int8 to Int8 requantization, required if the input and output tensors
    // have different scales and/or zero points.
    size_t size = ElementCount(*input->dims);
    switch (output->type) {
      case kTfLiteInt8:
        reference_ops::Requantize(tflite::micro::GetTensorData<int8_t>(input),
                                  size, data->output_multiplier,
                                  data->output_shift, data->input_zero_point,
                                  data->quantization_params.zero_point,
                                  tflite::micro::GetTensorData<int8_t>(output));
        break;
      default:
        TF_LITE_KERNEL_LOG(context, "Input %s, output %s not supported.",
                           TfLiteTypeGetName(input->type),
                           TfLiteTypeGetName(output->type));
        return kTfLiteError;
    }
  } else {
    TF_LITE_KERNEL_LOG(context, "Input %s, output %s not supported.",
                       TfLiteTypeGetName(input->type),
                       TfLiteTypeGetName(output->type));
    return kTfLiteError;
  }

  return kTfLiteOk;
}

TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
#if defined(CEVA_BX1) || defined(CEVA_SP500)
  return EvalCEVA(context, node);
#else
  return EvalQuantizeReference(context, node);
#endif
}

}  // namespace

// This Op (QUANTIZE) quantizes the input and produces quantized output.
// AffineQuantize takes scale and zero point and quantizes the float value to
// quantized output, in int8_t or uint8_t format.
TfLiteRegistration Register_QUANTIZE() {
  return {/*init=*/Init,
          /*free=*/nullptr,
          /*prepare=*/Prepare,
          /*invoke=*/Eval,
          /*profiling_string=*/nullptr,
          /*builtin_code=*/0,
          /*custom_name=*/nullptr,
          /*version=*/0};
}

}  // namespace tflite
