//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#define _CRT_SECURE_NO_WARNINGS // "secure" CRT not available on all platforms  --add this at the top of all CPP files that give "function or variable may be unsafe" warnings

#include "CNTKLibrary.h"
#include "CNTKLibraryHelpers.h"
#include "PlainTextDeseralizer.h"
#include "Layers.h"
#include "TimerUtility.h"

#include <cstdio>
#include <map>
#include <set>
#include <vector>

#define let const auto

using namespace CNTK;
using namespace std;

using namespace Dynamite;

// reference config [Arul]:
// \\mt-data\training_archive\mtmain_backend\rom_enu_generalnn\vCurrent\2017_06_10_00h_35m_31s\train_1\network_v3_src-3gru_tgt-1gru-4fcsc-1gru_coverage.xml
// differences:
//  - dropout not implemented yet
//  - GRU instead of LSTM
//  - ReLU not clipped/scaled
//  - no coverage/alignment model
//  - batch/length normalization
//  - no weight norm

const DeviceDescriptor device(DeviceDescriptor::UseDefaultDevice());
//const DeviceDescriptor device(DeviceDescriptor::GPUDevice(0));
//const DeviceDescriptor device(DeviceDescriptor::CPUDevice());
const size_t srcVocabSize = 27579 + 3;
const size_t tgtVocabSize = 21163 + 3;
const size_t embeddingDim = 512;
const size_t attentionDim = 512;
const size_t numEncoderLayers = 3;
const size_t encoderRecurrentDim = 512;
const size_t decoderRecurrentDim = 1024;
const size_t numDecoderResNetProjections = 4;
const size_t decoderProjectionDim = 768;
const size_t topHiddenProjectionDim = 1024;

size_t mbCount = 0; // made a global so that we can trigger debug information on it
#define DOLOG(var) (var)//((mbCount % 100 == 99) ? LOG(var) : 0)

BinarySequenceModel BidirectionalLSTMEncoder(size_t numLayers, size_t hiddenDim, double dropoutInputKeepProb)
{
    dropoutInputKeepProb;
    vector<BinarySequenceModel> layers;
    for (size_t i = 0; i < numLayers; i++)
        layers.push_back(Dynamite::Sequence::BiRecurrence(GRU(hiddenDim, device), Constant({ hiddenDim }, DTYPE, 0.0, device, L"fwdInitialValue"),
                                                          GRU(hiddenDim, device), Constant({ hiddenDim }, DTYPE, 0.0, device, L"bwdInitialValue")));
    vector<UnaryBroadcastingModel> bns;
    for (size_t i = 0; i < numLayers-1; i++)
        bns.push_back(Dynamite::BatchNormalization(device, Named("bnBidi")));
    vector<vector<Variable>> hs(2); // we need max. 2 buffers for the stack
    vector<Variable> hBn;
    vector<ModelParametersPtr> nested;
    nested.insert(nested.end(), layers.begin(), layers.end());
    nested.insert(nested.end(), bns.begin(), bns.end());
    return BinarySequenceModel(nested,
    [=](vector<Variable>& res, const vector<Variable>& xFwd, const vector<Variable>& xBwd) mutable
    {
        for (size_t i = 0; i < numLayers; i++)
        {
            const vector<Variable>& inFwd = (i == 0) ? xFwd : hs[i % 2];
            const vector<Variable>& inBwd = (i == 0) ? xBwd : hs[i % 2];
            vector<Variable>& out = (i == numLayers - 1) ? res : hs[(i+1) % 2];
            if (i > 0)
            {
                layers[i](hBn, inFwd, inBwd);
                bns[i - 1](out, hBn);
                hBn.clear();
            }
            else
                layers[i](out, inFwd, inBwd);
        }
        hs[0].clear(); hs[1].clear();
    });
}

// Bahdanau attention model
// (query, keys as tensor, data sequence as tensor) -> interpolated data vector
//  - keys used for the weights
//  - data gets interpolated
// Here they are the same.
TernaryModel AttentionModelBahdanau(size_t attentionDim1)
{
    auto Q = Parameter({ attentionDim1, NDShape::InferredDimension }, DTYPE, GlorotUniformInitializer(), device, L"Q"); // query projection
    auto v = Parameter({ attentionDim1 }, DTYPE, GlorotUniformInitializer(), device, L"v"); // tanh projection
    let normQ = LengthNormalization(device);
    return TernaryModel({ Q, /*K,*/ v }, { { L"normQ", normQ } },
    [=](const Variable& query, const Variable& projectedKeys/*keys*/, const Variable& data) -> Variable
    {
        // compute attention weights
        let projectedQuery = normQ(Times(Q, query, Named("Q"))); // [A x 1]
        let tanh = Tanh((projectedQuery + projectedKeys), Named("attTanh")); // [A x T]
#if 0
        let u = InnerProduct(tanh, v, Axis(0), Named("vProj")); // [1 x T] col vector
        let w = Dynamite::Softmax(u, Axis(1));
        let res = Reshape(InnerProduct(data, w, Axis(1), Named("att")), NDShape{ attentionDim1 }); // [A]
#else
        let u = TransposeTimes(tanh, v, Named("vProj")); // [T] col vector
        let w = Dynamite::Softmax(u);
        let res = Times(data, w, Named("att")); // [A]
#endif
        return res;
     });
}

// reference attention model
QuaternaryModel AttentionModelReference(size_t attentionDim1)
{
    auto H = Parameter({ attentionDim1, NDShape::InferredDimension }, DTYPE, GlorotUniformInitializer(), device, L"Q"); // query projection
    let normH = LengthNormalization(device);
    return QuaternaryModel({ H }, { { L"normH", normH } },
        [=](const Variable& h, const Variable& historyProjectedKey, const Variable& encodingProjectedNormedKey, const Variable& encodingProjectedData) -> Variable
    {
        // compute attention weights
        let hProjected = Times(H, h, Named("H")); // [A x 1]
        let tanh = Tanh(normH(hProjected + historyProjectedKey), Named("attTanh")); // [A x T]
        let u = ReduceSum(ElementTimes(tanh, Tanh(encodingProjectedNormedKey), Named("attDot")), Axis(0))->Output(); // [1 x T] col vector
        let w = Reshape(Dynamite::Softmax(u), { u.Shape().TotalSize() }); // [T] this is a transposition (Transpose does not work yet)
        let res = Times(encodingProjectedData, w, Named("attContext")); // [A x T] x [T] -> [A]
        return res;
    });
}

BinarySequenceModel AttentionDecoder(double dropoutInputKeepProb)
{
    // create all the layer objects
    let encBarrier = Barrier(L"encBarrier");
    let encoderKeysProjection = encBarrier >> Linear(attentionDim, false, device) >> BatchNormalization(device, Named("bnEncoderKeysProjection")); // keys projection for attention
    let encoderDataProjection = encBarrier >> Linear(attentionDim, false, device) >> BatchNormalization(device, Named("bnEncoderDataProjection")); // data projection for attention
    let embedTarget = Barrier(L"embedTargetBarrier") >> Embedding(embeddingDim, device) >> BatchNormalization(device, L"bnEmbedTarget");     // target embeddding
    let initialContext = Constant({ attentionDim }, DTYPE, 0.0, device, L"initialContext"); // 2 * because bidirectional --TODO: can this be inferred?
    let initialStateProjection = Dense(decoderRecurrentDim, UnaryModel([](const Variable& x) { return Tanh(x); }), device);
    let stepFunction = GRU(decoderRecurrentDim, device);
    //let attentionModel = AttentionModelBahdanau(attentionDim);
    let attentionModel = AttentionModelReference(attentionDim);
    let firstHiddenProjection = Barrier(Named("projBarrier")) >> Dense(decoderProjectionDim, UnaryModel([](const Variable& x) { return ReLU(x); }), device);
    vector<UnaryBroadcastingModel> resnets;
    for (size_t n = 0; n < numDecoderResNetProjections; n++)
        resnets.push_back(ResidualNet(decoderProjectionDim, device));
    let topHiddenProjection = Dense(topHiddenProjectionDim, UnaryModel([](const Variable& x) { return Tanh(x); }), device);
    let outputProjection = Linear(tgtVocabSize, device);  // output layer without non-linearity (no sampling yet)

    // buffers
    vector<Variable> encodingProjectedKeys, encodingProjectedData;

    // decode from a top layer of an encoder, using history as history
    map<wstring, ModelParametersPtr> nestedLayers =
    {
        { L"encoderKeysProjection",  encoderKeysProjection },
        { L"encoderDataProjection",  encoderDataProjection },
        { L"embedTarget",            embedTarget },
        { L"initialStateProjection", initialStateProjection },
        { L"stepFunction",           stepFunction },
        { L"attentionModel",         attentionModel },
        { L"firstHiddenProjection",  firstHiddenProjection },
        { L"topHiddenProjection",    topHiddenProjection },
        { L"outputProjection",       outputProjection },
    };
    for (let& resnet : resnets)
        nestedLayers[L"resnet[" + std::to_wstring(nestedLayers.size()) + L"]"] = resnet;
    return BinarySequenceModel({ }, nestedLayers,
    [=](vector<Variable>& res, const vector<Variable>& history, const vector<Variable>& hEncs) mutable
    {
        res.resize(history.size());
        // decoding loop
        Variable state = Slice(hEncs.front(), Axis(0), encoderRecurrentDim, 2 * encoderRecurrentDim); // initial state for the recurrence is the final encoder state of the backward recurrence
        state = initialStateProjection(state);      // match the dimensions
        Variable attentionContext = initialContext; // note: this is almost certainly wrong
        // common subexpression of attention.
        // We pack the result into a dense matrix; but only after the matrix product, to allow for it to be batched.
        encoderKeysProjection(encodingProjectedKeys, hEncs);
        encoderDataProjection(encodingProjectedData, hEncs);
        let encodingProjectedKeysTensor = Splice(encodingProjectedKeys, Axis(1), Named("encodingProjectedKeysTensor"));  // [A x T]
        encodingProjectedKeys.clear();
        let encodingProjectedDataTensor = Splice(encodingProjectedData, Axis(1), Named("encodingProjectedDataTensor"));  // [A x T]
        encodingProjectedData.clear();
        for (size_t t = 0; t < history.size(); t++)
        {
            // do recurrent step (in inference, history[t] would become res[t-1])
            let historyProjectedKey = embedTarget(history[t]);
            let input = Splice({ historyProjectedKey, attentionContext }, Axis(0), Named("augInput"));
            state = stepFunction(state, input);
            // compute attention vector
            //attentionContext = attentionModel(state, /*keys=*/projectedKeys, /*data=*/hEncsTensor);
            attentionContext = attentionModel(state, historyProjectedKey, encodingProjectedKeysTensor, encodingProjectedDataTensor);
            // stack of non-recurrent projections
            auto state1 = firstHiddenProjection(state); // first one brings it into the right dimension
            for (auto& resnet : resnets)                // then a bunch of ResNet layers
                state1 = resnet(state1);
            // one more transform, bringing back the attention context
            let topHidden = topHiddenProjection(Splice({ state1, attentionContext }, Axis(0)));
            // TODO: dropout layer here
            dropoutInputKeepProb;
            // compute output
            let z = outputProjection(topHidden);
            res[t] = z;
        }
    });
}

BinarySequenceModel CreateModelFunction()
{
    auto embedFwd = Embedding(embeddingDim, device) >> BatchNormalization(device, Named("bnEmbedFwd"));
    auto embedBwd = Embedding(embeddingDim, device) >> BatchNormalization(device, Named("bnEmbedBwd"));
    auto encode = BidirectionalLSTMEncoder(numEncoderLayers, encoderRecurrentDim, 0.8);
    auto decode = AttentionDecoder(0.8);
    vector<Variable> eFwd,eBwd, h;
    return BinarySequenceModel({},
    {
        { L"embedSourceFwd", embedFwd },
        { L"embedSourceBwd", embedBwd },
        { L"encode",         encode   },
        { L"decode",         decode   }
    },
    [=](vector<Variable>& res, const vector<Variable>& x, const vector<Variable>& history) mutable
    {
        // embedding
        embedFwd(eFwd, x);
        embedBwd(eBwd, x);
        // encoder
        encode(h, eFwd, eBwd);
        eFwd.clear(); eBwd.clear();
        // decoder (outputting logprobs of words)
        decode(res, history, h);
        h.clear();
    });
}

BinaryFoldingModel CreateCriterionFunction(const BinarySequenceModel& model_fn)
{
    vector<Variable> features, history, labels, losses;
    // features and labels are tensors with first dimension being the length
    BinaryModel criterion = [=](const Variable& featuresAsTensor, const Variable& labelsAsTensor) mutable -> Variable
    {
        // convert sequence tensors into sequences of tensors
        // and strip the corresponding boundary markers
        //  - features: strip any?
        //  - labels: strip leading <s>
        //  - history: strip training </s>
        as_vector(features, featuresAsTensor);
        as_vector(history, labelsAsTensor);
        labels.assign(history.begin() + 1, history.end()); // make a full copy (of single-word references) without leading <s>
        history.pop_back(); // remove trailing </s>
        // apply model function
        // returns the sequence of output log probs over words
        vector<Variable> z;
        model_fn(z, features, history);
        features.clear(); history.clear(); // free some GPU memory
        // compute loss per word
        let sequenceLoss = Dynamite::Sequence::Map(BinaryModel([](const Variable& z, const Variable& label) { return Dynamite::CrossEntropyWithSoftmax(z, label); }));
        sequenceLoss(losses, z, labels);
        let loss = Batch::sum(losses); // TODO: Batch is not the right namespace; but this does the right thing
        labels.clear(); losses.clear();
        return loss;
    };
    // create a batch mapper (which will eventually allow suspension)
    let batchModel = Batch::Map(criterion);
    // for final summation, we create a new lambda (featBatch, labelBatch) -> mbLoss
    vector<Variable> lossesPerSequence;
    return BinaryFoldingModel({}, { { L"model", model_fn } },
    [=](const /*batch*/vector<Variable>& features, const /*batch*/vector<Variable>& labels) mutable -> Variable
    {
        batchModel(lossesPerSequence, features, labels);             // batch-compute the criterion
        let collatedLosses = Splice(lossesPerSequence, Axis(0), Named("cesPerSeq"));     // collate all seq lossesPerSequence
        let mbLoss = ReduceSum(collatedLosses, Axis(0), Named("ceBatch"));  // aggregate over entire minibatch
        lossesPerSequence.clear();
        return mbLoss;
    });
}

void Train()
{
    // dynamic model and criterion function
    auto model_fn = CreateModelFunction();
    auto criterion_fn = CreateCriterionFunction(model_fn);

    // data
    auto minibatchSourceConfig = MinibatchSourceConfig({ PlainTextDeserializer(
        {
        //PlainTextStreamConfiguration(L"src", srcVocabSize, { L"d:/work/Karnak/sample-model/data/train.src" }, { L"d:/work/Karnak/sample-model/data/vocab.src", L"<s>", L"</s>", L"<unk>" }),
        //PlainTextStreamConfiguration(L"tgt", tgtVocabSize, { L"d:/work/Karnak/sample-model/data/train.tgt" }, { L"d:/work/Karnak/sample-model/data/vocab.tgt", L"<s>", L"</s>", L"<unk>" })
        PlainTextStreamConfiguration(L"src", srcVocabSize, { L"f:/hanyh-ws2/shared/forFrank/ROM-ENU-WMT/Data/corpus.bpe.ro.shuf" }, { L"f:/hanyh-ws2/shared/forFrank/ROM-ENU-WMT/Data/corpus.bpe.ro.vocab", L"<s>", L"</s>", L"<unk>" }),
        PlainTextStreamConfiguration(L"tgt", tgtVocabSize, { L"f:/hanyh-ws2/shared/forFrank/ROM-ENU-WMT/Data/corpus.bpe.en.shuf" }, { L"f:/hanyh-ws2/shared/forFrank/ROM-ENU-WMT/Data/corpus.bpe.en.vocab", L"<s>", L"</s>", L"<unk>" })
    }) },
        /*randomize=*/true);
    minibatchSourceConfig.maxSamples = MinibatchSource::InfinitelyRepeat;
    let minibatchSource = CreateCompositeMinibatchSource(minibatchSourceConfig);
    // BUGBUG (API): no way to specify MinibatchSource::FullDataSweep in a single expression

    // run something through to get the parameter matrices shaped --ugh!
    vector<Variable> d1{ Constant({ srcVocabSize }, DTYPE, 0.0, device) };
    vector<Variable> d2{ Constant({ tgtVocabSize }, DTYPE, 0.0, device) };
    vector<Variable> d3;
    model_fn(d3, d1, d2);

    model_fn.LogParameters();

    let parameters = model_fn.Parameters();
    size_t numParameters = 0;
    for (let& p : parameters)
        numParameters += p.Shape().TotalSize();
    fprintf(stderr, "Total number of learnable parameters: %u\n", (unsigned int)numParameters);
    let epochSize = 10000000; // 10M is a bit more than half an epoch of ROM-ENG (~16M words)
    let minibatchSize = 4096;
    AdditionalLearningOptions learnerOptions;
    learnerOptions.gradientClippingThresholdPerSample = 0.2;
#if 0
    auto baseLearner = SGDLearner(parameters, LearningRatePerSampleSchedule(0.0005), learnerOptions);
#else
    // AdaGrad correction-correction:
    //  - LR is specified for av gradient
    //  - numer should be /minibatchSize
    //  - denom should be /sqrt(minibatchSize)
    let f = 1 / sqrt(minibatchSize)/*AdaGrad correction-correction*/;
    let lr0 = 0.0003662109375 * f;
    auto baseLearner = AdamLearner(parameters, LearningRatePerSampleSchedule({ lr0, lr0/2, lr0/4, lr0/8 }, epochSize),
        MomentumAsTimeConstantSchedule(40000), true, MomentumAsTimeConstantSchedule(400000), /*eps=*/1e-8, /*adamax=*/false,
        learnerOptions);
#endif
    let communicator = MPICommunicator();
    let& learner = CreateDataParallelDistributedLearner(communicator, baseLearner, /*distributeAfterSamples =*/ 0, /*useAsyncBufferedParameterUpdate =*/ false);
    unordered_map<Parameter, NDArrayViewPtr> gradients;
    for (let& p : parameters)
        gradients[p] = nullptr;

    vector<vector<Variable>> args;
    size_t totalLabels = 0;
    class SmoothedVar
    {
        double smoothedNumer = 0; double smoothedDenom = 0;
        const double smoothedDecay = 0.99;
    public:
        double Update(double avgVal, size_t count)
        {
            // TODO: implement the correct smoothing
            smoothedNumer = smoothedDecay * smoothedNumer + (1 - smoothedDecay) * avgVal * count;
            smoothedDenom = smoothedDecay * smoothedDenom + (1 - smoothedDecay) * count;
            return Value();
        }
        double Value() const
        {
            return smoothedNumer / smoothedDenom;
        }
    } smoothedLoss;
    Microsoft::MSR::CNTK::Timer timer;
    class // helper for timing GPU-side operations
    {
        bool enabled = false; // set to true to enable, false to disable
        Microsoft::MSR::CNTK::Timer m_timer;
        void syncGpu() { CNTK::NDArrayView::Sync(device); }
    public:
        void Restart()
        {
            if (enabled)
            {
                syncGpu();
                m_timer.Restart();
            }
        }
        void Log(const char* what, size_t numItems)
        {
            if (enabled)
            {
                syncGpu();
                let elapsed = m_timer.ElapsedSeconds();
                fprintf(stderr, "%s: %d items, items/sec=%.2f, ms/item=%.2f\n", what, (int)numItems, numItems / elapsed, 1000.0/*ms*/ * elapsed / numItems);
            }
        }
    } partTimer;
    wstring modelPath = L"d:/me/tmp_dynamite_model.cmf";
    size_t saveEvery = 100;
    size_t startMbCount = 0;
    if (startMbCount > 0)
    {
        // restarting after crash. Note: not checkpointing the reader yet.
        let path = modelPath + L"." + to_wstring(startMbCount);
        fprintf(stderr, "restarting from: %S\n", path.c_str());
        model_fn.RestoreParameters(path);
    }
    for (mbCount = startMbCount; true; mbCount++)
    {
        // checkpoint
        if (mbCount % saveEvery == 0 &&
            (startMbCount == 0 || mbCount > startMbCount) && // don't overwrite the starting model
            communicator->CurrentWorker().IsMain())
        {
            let path = modelPath + L"." + to_wstring(mbCount);
            fprintf(stderr, "saving: %S\n", path.c_str());
            model_fn.SaveParameters(path);
            // test model saving
            //for (auto& param : parameters) // destroy parameters as to prove that we reloaded them correctly.
            //    param.Value()->SetValue(0.0);
            //model_fn.RestoreParameters(path);
        }
        timer.Restart();
        // get next minibatch
        partTimer.Restart();
        auto minibatchData = minibatchSource->GetNextMinibatch(/*minibatchSizeInSequences=*/ (size_t)0, (size_t)minibatchSize, communicator->Workers().size(), communicator->CurrentWorker().m_globalRank, device);
        if (minibatchData.empty()) // finished one data pass--TODO: really? Depends on config. We really don't care about data sweeps.
            break;
        let numLabels = minibatchData[minibatchSource->StreamInfo(L"tgt")].numberOfSamples;
        partTimer.Log("GetNextMinibatch", numLabels);
        fprintf(stderr, "#seq: %d, #words: %d, lr=%.8f\n",
                (int)minibatchData[minibatchSource->StreamInfo(L"src")].numberOfSequences,
                (int)minibatchData[minibatchSource->StreamInfo(L"src")].numberOfSamples,
                learner->LearningRate());
        partTimer.Restart();
        Dynamite::FromCNTKMB(args, { minibatchData[minibatchSource->StreamInfo(L"src")].data, minibatchData[minibatchSource->StreamInfo(L"tgt")].data }, { true, true }, DTYPE, device);
        partTimer.Log("FromCNTKMB", numLabels);
#if 0   // for debugging: reduce #sequences to 3, and reduce their lengths
        args[0].resize(3);
        args[1].resize(3);
        let TrimLength = [](Variable& seq, size_t len) // chop off all frames after 'len', assuming the last axis is the length
        {
            seq = Slice(seq, Axis((int)seq.Shape().Rank()-1), 0, (int)len);
        };
        // input
        TrimLength(args[0][0], 2);
        TrimLength(args[0][1], 4);
        TrimLength(args[0][2], 3);
        // target
        TrimLength(args[1][0], 3);
        TrimLength(args[1][1], 2);
        TrimLength(args[1][2], 2);
#endif
        // train minibatch
        partTimer.Restart();
        let mbLoss = criterion_fn(args[0], args[1]);
        partTimer.Log("criterion_fn", numLabels);
        // backprop and model update
        partTimer.Restart();
        mbLoss.Value()->AsScalar<float>();
        partTimer.Log("ForwardProp", numLabels);
        partTimer.Restart();
        mbLoss.Backward(gradients);
        partTimer.Log("BackProp", numLabels);
        mbLoss.Value()->AsScalar<float>();
        MinibatchInfo info{ /*atEndOfData=*/false, /*sweepEnd=*/false, /*numberOfSamples=*/numLabels, mbLoss.Value(), mbLoss.Value() };
        info.trainingLossValue->AsScalar<float>();
        partTimer.Restart();
        learner->Update(gradients, info);
        partTimer.Log("Update", numLabels);
        let lossPerLabel = info.trainingLossValue->AsScalar<float>() / info.numberOfSamples; // note: this does the GPU sync, so better do that only every N
        totalLabels += info.numberOfSamples;
        // I once saw a strange (impossible) -1e23 or so CE loss, no idea where that comes from. Skip it in smoothed loss. Does not seem to hurt the convergence.
        if (lossPerLabel < 0)
        {
            fprintf(stderr, "%d STRANGE CrossEntropy loss = %.7f, not counting in accumulative loss, seenLabels=%d, words/sec=%.1f\n",
                (int)mbCount, lossPerLabel, (int)totalLabels,
                info.numberOfSamples / timer.ElapsedSeconds());
            continue;
        }
        let smoothedLossVal = smoothedLoss.Update(lossPerLabel, info.numberOfSamples);
        let elapsed = timer.ElapsedSeconds(); // [sec]
        fprintf(stderr, "%d: CrossEntropy loss = %.7f; PPL = %.3f; smLoss = %.7f, smPPL = %.2f, seenLabels=%d, words/sec=%.1f, ms/word=%.1f\n",
                        (int)mbCount, lossPerLabel, exp(lossPerLabel), smoothedLossVal, exp(smoothedLossVal), (int)totalLabels,
                        info.numberOfSamples / elapsed, 1000.0/*ms*/ * elapsed / info.numberOfSamples);
        // log
        // Do this last, which forces a GPU sync and may avoid that "cannot resize" problem
        if (mbCount < 400 || mbCount % 5 == 0)
            fflush(stderr);
        //if (mbCount == 20) // for mem leak check
        //    break;
        if (std::isnan(lossPerLabel))
            throw runtime_error("Loss is NaN.");
        //if (mbCount == 2)
        //    exit(0);
    }
}


int mt_main(int argc, char *argv[])
{
    argc; argv;
    try
    {
        Train();
        //Train(DeviceDescriptor::CPUDevice(), true);
    }
    catch (exception& e)
    {
        fprintf(stderr, "EXCEPTION caught: %s\n", e.what());
        fflush(stderr);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
