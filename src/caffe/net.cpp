#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <numeric>

#include "caffe/common.hpp"
#include "caffe/layer.hpp"
#include "caffe/net.hpp"
#include "caffe/solver.hpp"
#include "caffe/blob_solver.hpp"
#include "caffe/proto/caffe.pb.h"
#include "caffe/util/insert_splits.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/upgrade_proto.hpp"

#include "caffe/test/test_caffe_main.hpp"

namespace caffe {

//template<typename Dtype>
//class NetThread;

template<typename Dtype>
Net<Dtype>::Net(const NetParameter& param, vector<int> device_ids,
		Solver<Dtype> *solver, SolverParameter solver_param) :
		solver_(solver), solver_param_(solver_param) {
	if (device_ids.size() > 0) {
		Caffe::SetDevice(device_ids[0]);
	}
	Init(param, device_ids);
}

template<typename Dtype>
Net<Dtype>::~Net() {
	for (int i = 0; i < net_output_blobs_.size(); ++i) {
		delete net_output_blobs_[i];
	}
}

template<typename Dtype>
Net<Dtype>::Net(const string& param_file, vector<int> device_ids,
		Solver<Dtype> *solver, SolverParameter solver_param) :
		solver_(solver), solver_param_(solver_param) {
	NetParameter param;
	ReadNetParamsFromTextFileOrDie(param_file, &param);
	Init(param, device_ids);
}

template<typename Dtype>
void Net<Dtype>::Init(const NetParameter& in_param, vector<int> &device_ids) {
	CHECK_GT(device_ids.size(), 0)<< "Must have at least one device id";
	device_ids_ = device_ids;
	for (int i = 0; i < device_ids_.size(); ++i) {
		LOG(INFO)<<"device_ids "<<device_ids_[i];
	}
	losses_.resize(device_ids.size());
	batch_size_ = 0;
	batch_sizes_.resize(device_ids.size());

	// Filter layers based on their include/exclude rules and
	// the current NetState.
	NetParameter filtered_param;
	FilterNet(in_param, &filtered_param);
	LOG(INFO)<< "Initializing net from parameters: " << std::endl
	<< filtered_param.DebugString();
	// Create a copy of filtered_param with splits added where necessary.
	NetParameter param;
	InsertSplits(filtered_param, &param);

	InitDataManager(param);
	InitNetThreads(param);
	ConnectReplicas();
}

template<typename Dtype>
void Net<Dtype>::PostInit() {
	net_output_blobs_.resize(net_threads_[0]->num_outputs());
	for(int i = 0;i < net_threads_[0]->num_outputs();++i) {
		// assume the net_output_blobs_ stores global accumulative quantities, such as accuracy, loss
		int num = net_threads_[0]->output_blobs()[i]->num();
		for(int j = 1;j < net_threads_.size();++j) {
			CHECK_EQ(num, net_threads_[j]->output_blobs()[i]->num());
		}
		Blob<Dtype> *b = new Blob<Dtype>(num, net_threads_[0]->output_blobs()[i]->channels(),
				net_threads_[0]->output_blobs()[i]->height(),
				net_threads_[0]->output_blobs()[i]->width());
		net_output_blobs_[i] = b;
	}

	batch_size_ = 0;
	for(int i=0;i<batch_sizes_.size();++i) {
		batch_size_ += batch_sizes_[i];
	}
	batch_size_ratios_.clear();
	for(int i=0;i<batch_sizes_.size();++i) {
		batch_size_ratios_[device_ids_[i]] = (Dtype)batch_sizes_[i] / (Dtype)batch_size_;
	}

	for(int i=0;i<net_threads_.size(); ++i) {
		net_threads_[i]->PostInit();
	}
	LOG(INFO)<<"Net<Dtype>::PostInit end";

//	data_manager_->CreatePrefetchThread();
}

template<typename Dtype>
void Net<Dtype>::InitDataManager(const NetParameter& param) {
	// Hack
	for (int layer_id = 0; layer_id < param.layer_size(); ++layer_id) {
		const LayerParameter& layer_param = param.layer(layer_id);

		if (layer_param.type() == std::string("Data")) {
			data_manager_.reset(new DataManager<Dtype>(layer_param, this));
			data_manager_->CreatePrefetchThread();
			break;
		}
	}

}

template<typename Dtype>
void Net<Dtype>::InitNetThreads(const NetParameter& param) {
	LOG(INFO)<<"solver train net "<<solver_param_.net();
	for (int i = 0; i < device_ids_.size(); ++i) {
		LOG(INFO)<<"Net<Dtype>::InitNetThreads "<<i;
		NetThread<Dtype> *p_thread = new NetThread<Dtype>(param, device_ids_[i], i,
				this, solver_param_);
		net_threads_.push_back(p_thread);
		const vector<shared_ptr<Layer<Dtype> > > &thread_layers =
		p_thread->layers();
		if (layer_map_.size() == 0) {
			layer_map_.resize(thread_layers.size());
		}
		for (int j = 0; j < thread_layers.size(); ++j) {
			layer_map_[j][i] = thread_layers[j];
		}
	}
}

template<typename Dtype>
void Net<Dtype>::ConnectReplicas() {
//	for (int i = 0; i < layer_map_.size(); ++i) {
//		int n_replicas = layer_map_[i].size();
//		for (int j = 0; j < n_replicas; ++j) {
//			Layer<Dtype> *l1 = layer_map_[i][j].get();
//			for (int k = 0; k < n_replicas; ++k) {
//				Layer<Dtype> *l2 = layer_map_[i][k].get();
//				l1->add_replica(l2);
//			}
//		}
//	}

	for (int i = 0; i < net_threads_.size(); ++i) {
		NetThread<Dtype> *nt1 = net_threads_[i];
		for (int j = 0; j < net_threads_.size(); ++j) {
			nt1->add_replicas(net_threads_[j]);
		}
	}
}

template<typename Dtype>
Dtype Net<Dtype>::ForwardBackward(vector<Blob<Dtype>*>& bottom) {
	DLOG(INFO)<<"Net<Dtype>::ForwardBackward";
	Dtype loss;
	ForwardBackwardHelper(bottom, &loss, true);
	return loss;
}

template<typename Dtype>
void Net<Dtype>::ForwardPrefilled(Dtype* loss) {
	data_manager_->Forward();
	for (int i = 0; i < net_threads_.size(); ++i) {
		net_threads_[i]->set_work_message(FORWARD_PREFILLED);
		net_threads_[i]->StartWork();
	}
	for (int i = 0; i < net_threads_.size(); ++i) {
		net_threads_[i]->FinishWork();
	}
	CollectLoss();
	*loss = std::accumulate(losses_.begin(), losses_.end(), 0);
}

template<typename Dtype>
const vector<Blob<Dtype>*>& Net<Dtype>::Forward(
		const vector<Blob<Dtype>*>& bottom, Dtype *loss) {
	ForwardBackwardHelper(bottom, loss, false);

//	for (int i = 0; i < net_threads_[0]->num_outputs(); ++i) {
//		Dtype *tgt_data = net_output_blobs_[i]->mutable_cpu_data();
//		caffe_memset(net_threads_[0]->output_blobs()[i]->count(), 0, tgt_data);
//		for (int j = 0; j < net_threads_.size(); ++j) {
//			const Dtype *src_data = net_threads_[j]->output_blobs()[i]->cpu_data();
//			for(int k = 0; k < net_threads_[0]->output_blobs()[i]->count(); ++k){
//				tgt_data[k] += batch_size_ratios_[device_ids_[j]] * src_data[k];
//			}
//		}
//	}
	return net_output_blobs_;
}

template<typename Dtype>
string Net<Dtype>::Forward(const string& input_blob_protos, Dtype* loss) {
	data_manager_->Forward();
	for (int i = 0; i < net_threads_.size(); ++i) {
		net_threads_[i]->set_input_blob_protos(input_blob_protos);
		net_threads_[i]->set_work_message(FORWARD_INPUT_BLOB_PROTOS);
		net_threads_[i]->StartWork();
//		net_threads_[i]->StartForwardInputBlobProtos(input_blob_protos);
	}
	for (int i = 0; i < net_threads_.size(); ++i) {
		net_threads_[i]->FinishWork();
//		losses_[i] = net_threads_[i]->FinishForwardInputBlobProtos();
	}
	CollectLoss();

	if (loss) {
		*loss = std::accumulate(losses_.begin(), losses_.end(), 0);
	}

	BlobProtoVector blob_proto_vec;

	for (int i = 0; i < net_threads_[0]->output_blobs().size(); ++i) {
		vector<Blob<Dtype>*> sources;
		for (int j = 0; j < net_threads_.size(); ++j) {
			const vector<Blob<Dtype>*>& thread_output_blobs =
					net_threads_[j]->output_blobs();
			sources.push_back(thread_output_blobs[i]);
		}
		Blob<Dtype> comb_blob;
		comb_blob.CopyFrom(sources, false, true);
		comb_blob.ToProto(blob_proto_vec.add_blobs());
	}
	string output;
	blob_proto_vec.SerializeToString(&output);
	return output;
}

template<typename Dtype>
const shared_ptr<Blob<Dtype> > Net<Dtype>::blob_by_name(
		const string& blob_name) {
	CHECK_GT(net_threads_.size(), 0);
	shared_ptr<Blob<Dtype> > blob_ptr;
	if (net_threads_[0]->has_blob(blob_name)) {
		vector<Blob<Dtype>*> thread_blobs;
		for (int i = 0; i < net_threads_.size(); ++i) {
			thread_blobs.push_back(net_threads_[i]->blob_by_name(blob_name).get());
		}
		blob_ptr.reset(new Blob<Dtype>());
		blob_ptr->CopyFrom(thread_blobs, false, true);
//
//
//		vector<const shared_ptr<Blob<Dtype> > > therad_blobs;
//		therad_blobs.resize(net_threads_.size());
//		int num=0;
//		for(int i=0;i<net_threads_.size();++i){
//			therad_blobs[i]=net_threads_[i]->blob_by_name(blob_name);
//			num+=therad_blobs[i]->num();
//		}
//		blob_ptr.reset(new Blob<Dtype>(num, therad_blobs[0]->channels(),
//				therad_blobs[0]->height(),therad_blobs[0]->width()));
//		Dtype* data = blob_ptr->mutable_cpu_data();
//		for(int i=0;i<net_threads_.size();++i){
//			memcpy(data, therad_blobs[i]->cpu_data(), therad_blobs[i]->count()*sizeof(Dtype));
//			data += blob_ptr->offset(therad_blobs[i]->num());
//		}
	} else {
		blob_ptr.reset((Blob<Dtype>*) (NULL));
		LOG(WARNING)<< "Unknown blob name " << blob_name;
	}
	return blob_ptr;
}

template<typename Dtype>
void Net<Dtype>::Backward() {
	for (int i = 0; i < net_threads_.size(); ++i) {
		net_threads_[i]->Backward();
	}
}

template<typename Dtype>
void Net<Dtype>::CollectLoss() {
//	for (int i = 0; i < 1; ++i) {
	for (int i = 0; i < net_threads_.size(); ++i) {
		losses_[i] = GetBatchSizeRatio(device_ids_[i])
				* net_threads_[i]->get_loss();
	}
}

template<typename Dtype>
void Net<Dtype>::CopyTrainedLayersFrom(const NetParameter& param) {
	for (int i = 0; i < net_threads_.size(); ++i) {
		net_threads_[i]->CopyTrainedLayersFrom(param);
	}
}

/*
 * TO DO
 * update when a net is directly initialized from here
 * how many device ids?
 * */
template<typename Dtype>
void Net<Dtype>::CopyTrainedLayersFrom(const string trained_filename) {
	for (int i = 0; i < net_threads_.size(); ++i) {
		net_threads_[i]->CopyTrainedLayersFrom(trained_filename);
	}
}

template<typename Dtype>
void Net<Dtype>::ToProto(NetParameter* param, bool write_diff) const {
	CHECK_GT(net_threads_.size(), 0);
	net_threads_[0]->ToProto(param, write_diff);
}

template<typename Dtype>
void Net<Dtype>::FilterNet(const NetParameter& param,
		NetParameter* param_filtered) {
	NetState net_state(param.state());
	// Let the phase of the net be the current global phase provided in the Caffe
	// singleton, unless explicitly provided by the state.
	if (!net_state.has_phase()) {
		switch (Caffe::phase()) {
		case Caffe::TRAIN:
			net_state.set_phase(TRAIN);
			break;
		case Caffe::TEST:
			net_state.set_phase(TEST);
			break;
		default:
			LOG(FATAL)<< "Unknown phase: " << Caffe::phase();
		}
	}
	param_filtered->CopyFrom(param);
	param_filtered->clear_layer();
	for (int i = 0; i < param.layer_size(); ++i) {
		const LayerParameter& layer_param = param.layer(i);
		const string& layer_name = layer_param.name();
		CHECK(layer_param.include_size() == 0 || layer_param.exclude_size() == 0)
																																									<< "Specify either include rules or exclude rules; not both.";
		// If no include rules are specified, the layer is included by default and
		// only excluded if it meets one of the exclude rules.
		bool layer_included = (layer_param.include_size() == 0);
		for (int j = 0; layer_included && j < layer_param.exclude_size(); ++j) {
			if (StateMeetsRule(net_state, layer_param.exclude(j), layer_name)) {
				layer_included = false;
			}
		}
		for (int j = 0; !layer_included && j < layer_param.include_size(); ++j) {
			if (StateMeetsRule(net_state, layer_param.include(j), layer_name)) {
				layer_included = true;
			}
		}
		if (layer_included) {
			param_filtered->add_layer()->CopyFrom(layer_param);
		}
	}
}

template<typename Dtype>
bool Net<Dtype>::StateMeetsRule(const NetState& state, const NetStateRule& rule,
		const string& layer_name) {
	// Check whether the rule is broken due to phase.
	if (rule.has_phase()) {
		if (rule.phase() != state.phase()) {
			LOG(INFO)<< "The NetState phase (" << state.phase()
			<< ") differed from the phase (" << rule.phase()
			<< ") specified by a rule in layer " << layer_name;
			return false;
		}
	}
	// Check whether the rule is broken due to min level.
	if (rule.has_min_level()) {
		if (state.level() < rule.min_level()) {
			LOG(INFO) << "The NetState level (" << state.level()
			<< ") is above the min_level (" << rule.min_level()
			<< ") specified by a rule in layer " << layer_name;
			return false;
		}
	}
	// Check whether the rule is broken due to max level.
	if (rule.has_max_level()) {
		if (state.level() > rule.max_level()) {
			LOG(INFO) << "The NetState level (" << state.level()
			<< ") is above the max_level (" << rule.max_level()
			<< ") specified by a rule in layer " << layer_name;
			return false;
		}
	}
	// Check whether the rule is broken due to stage. The NetState must
	// contain ALL of the rule's stages to meet it.
	for (int i = 0; i < rule.stage_size(); ++i) {
		// Check that the NetState contains the rule's ith stage.
		bool has_stage = false;
		for (int j = 0; !has_stage && j < state.stage_size(); ++j) {
			if (rule.stage(i) == state.stage(j)) {has_stage = true;}
		}
		if (!has_stage) {
			LOG(INFO) << "The NetState did not contain stage '" << rule.stage(i)
			<< "' specified by a rule in layer " << layer_name;
			return false;
		}
	}
	// Check whether the rule is broken due to not_stage. The NetState must
	// contain NONE of the rule's not_stages to meet it.
	for (int i = 0; i < rule.not_stage_size(); ++i) {
		// Check that the NetState contains the rule's ith not_stage.
		bool has_stage = false;
		for (int j = 0; !has_stage && j < state.stage_size(); ++j) {
			if (rule.not_stage(i) == state.stage(j)) {has_stage = true;}
		}
		if (has_stage) {
			LOG(INFO) << "The NetState contained a not_stage '" << rule.not_stage(i)
			<< "' specified by a rule in layer " << layer_name;
			return false;
		}
	}
	return true;
}

template<typename Dtype>
void Net<Dtype>::set_debug_info(const bool value) {
	for (int i = 0; i < net_threads_.size(); ++i) {
		net_threads_[i]->set_debug_info(value);
	}
}

template<typename Dtype>
std::string Net<Dtype>::name() {
	CHECK_GT(net_threads_.size(), 0);
	return net_threads_[0]->name();
}

template<typename Dtype>
void Net<Dtype>::ComputeUpdateValue() {
	for(int i = 0; i< net_threads_.size(); ++i){
		Caffe::SetDevice(net_threads_[i]->get_device_id());
		Caffe::SyncDevice();
	}

	for (int i = 0; i < net_threads_.size(); ++i) {
		net_threads_[i]->set_work_message(AGGREGATE_GRADIENT);
		net_threads_[i]->StartWork();
	}
	for (int i = 0; i < net_threads_.size(); ++i) {
		net_threads_[i]->FinishWork();
	}

	for(int i = 0; i< net_threads_.size(); ++i){
		Caffe::SetDevice(net_threads_[i]->get_device_id());
		Caffe::SyncDevice();
	}

	for (int i = 0; i < net_threads_.size(); ++i) {
		net_threads_[i]->set_work_message(COMPUTE_UPDATE_VALUE);
		net_threads_[i]->StartWork();
	}

	for (int i = 0; i < net_threads_.size(); ++i) {
		net_threads_[i]->FinishWork();
	}
}

template<typename Dtype>
void Net<Dtype>::Update() {
	for(int i = 0; i< net_threads_.size(); ++i){
		Caffe::SetDevice(net_threads_[i]->get_device_id());
		Caffe::SyncDevice();
	}

	for (int i = 0; i < net_threads_.size(); ++i) {
		net_threads_[i]->set_work_message(UPDATE_WEIGHTS);
		net_threads_[i]->StartWork();
	}
	for (int i = 0; i < net_threads_.size(); ++i) {
		net_threads_[i]->FinishWork();
	}

}

template<typename Dtype>
void Net<Dtype>::ShareTrainedLayersWith(const Net<Dtype>* other) {
	CHECK_EQ(net_threads_.size(), other->GetNetThreads().size());
	for (int i = 0; i < net_threads_.size(); ++i) {
		net_threads_[i]->ShareTrainedLayersWith(other->GetNetThreads()[i]);
	}
}

template<typename Dtype>
void Net<Dtype>::ForwardBackwardHelper(const vector<Blob<Dtype>*>& bottom,
		Dtype *loss, bool do_backward) {
	DLOG(INFO)<<"Net<Dtype>::ForwardBackwardHelper bottom.size "<<bottom.size();
	data_manager_->Forward();
//	for (int i = 0; i < 1; ++i) {
	for (int i = 0; i < net_threads_.size(); ++i) {
		vector<Blob<Dtype>* > thread_bottom(bottom.size(), 0);
		int old_device = Caffe::GetDeviceId();
		Caffe::SetDevice(net_threads_[i]->get_device_id());
		for (int j = 0; j < bottom.size(); ++j) {
			int num = bottom[j]->num();
			int n_thread = net_threads_.size();
			int thread_batch_size = (num + n_thread - 1) / n_thread;
			int start = j * thread_batch_size;
			int end = std::min((j + 1) * thread_batch_size, num);

			Blob<Dtype> *b = new Blob<Dtype>(end - start, bottom[j]->channels(),
					bottom[j]->height(), bottom[j]->width());
			b->set_cpu_data(bottom[j]->mutable_cpu_data() + bottom[j]->offset(start));
			thread_bottom.push_back(b);
		}
		Caffe::SetDevice(old_device);

		net_threads_[i]->set_bottom(thread_bottom);
		if (do_backward) {
			net_threads_[i]->set_work_message(FORWARD_BACKWARD);
		} else {
			net_threads_[i]->set_work_message(FORWARD);
		}
		net_threads_[i]->StartWork();
//		net_threads_[i]->StartForwardBackward(thread_bottom.get(), do_backward);
	}

//	for (int i = 0; i < 1; ++i) {
	for (int i = 0; i < net_threads_.size(); ++i) {
		net_threads_[i]->FinishWork();
//		losses_[i] = net_threads_[i]->FinishForwardBackward();
	}

	// update net net_output_blobs_
	for (int i = 0; i < net_threads_[0]->num_outputs(); ++i) {
		Dtype *tgt_data = net_output_blobs_[i]->mutable_cpu_data();
		caffe_memset(sizeof(Dtype) * net_threads_[0]->output_blobs()[i]->count(), 0, tgt_data);
		for(int j = 0; j < net_threads_[0]->output_blobs()[i]->count(); ++j) {

			for(int k = 0; k < net_threads_.size(); ++k) {
				const Dtype *src_data = net_threads_[k]->output_blobs()[i]->cpu_data();
				tgt_data[j] += batch_size_ratios_[device_ids_[k]] * src_data[j];
			}
		}
//		for (int j = 0; j < net_threads_.size(); ++j) {
//			const Dtype *src_data = net_threads_[j]->output_blobs()[i]->cpu_data();
//			for(int k = 0; k < net_threads_[0]->output_blobs()[i]->count(); ++k){
//				tgt_data[k] += (batch_size_ratios_[device_ids_[j]] * src_data[k]);
//				LOG(INFO)<<"Net<Dtype>::ForwardBackwardHelper net_thread "<<j<<
//						" k "<<k<<" batch_size_ratio "<< batch_size_ratios_[device_ids_[j]] <<
//						" val "<< src_data[k];
//
//			}
//		}
	}

	CollectLoss();
	if (loss) {
		*loss = std::accumulate(losses_.begin(), losses_.end(), (Dtype)0.0);
	}
}

INSTANTIATE_CLASS(Net);

template<typename Dtype>
NetThread<Dtype>::NetThread(const NetParameter& param, int device_id,
		int replica_id, Net<Dtype>* net, const SolverParameter &solver_param) :
		device_id_(device_id), replica_id_(replica_id), net_(net){
	Caffe::SetDevice(device_id);

//	solver_.reset(GetNetThreadSolver<Dtype>(solver_param, this));

//	blob_diff_reducer_.reset(new BlobDiffReducer<Dtype>(this));
//	blob_diff_broadcaster_.reset(
//			IBroadcastDiffNetwork<Dtype>::make(net_->GetDeviceIds(), device_id_));

	Init(param);
}

template<typename Dtype>
void NetThread<Dtype>::InitCuda() {
	Caffe::SetDevice(device_id_);
	CUDA_CHECK(cudaDeviceSetCacheConfig(cudaFuncCachePreferShared));
	for (int i = 0; i < net_->GetDeviceIds().size(); ++i) {
		int d = net_->GetDeviceIds()[i];
		if (d != device_id_) {
			if (Caffe::CanAccessPeer(device_id_, d)) {
				LOG(INFO)<<"Enable peer access GPU "<<device_id_<<" --> GPU "<<d;
				cudaError_t err = cudaDeviceEnablePeerAccess(d, 0);
				CHECK_EQ( (err == cudaSuccess) || (err == cudaErrorPeerAccessAlreadyEnabled), 1);
			}
			else {
				LOG(INFO)<<"No peer access GPU "<<device_id_<<" --> GPU "<<d;
			}
		}
	}
}

template<typename Dtype>
void NetThread<Dtype>::Init(const NetParameter& param) {

	// Basically, build all the layers and set up its connections.
//	int n_replicas = net_->GetDeviceIds().size();
	name_ = param.name();
	test_net_ = param.state().phase() == 1 ? true : false;
	LOG(INFO)<<"NetThread<Dtype>::Init test_net_ "<<test_net_;

	map<string, int> blob_name_to_idx;
	set<string> available_blobs;
	CHECK_EQ(param.input_size() * 4, param.input_dim_size())<< "Incorrect input blob dimension specifications.";
	memory_used_ = 0;
	// set the input blobs
	for (int input_id = 0; input_id < param.input_size(); ++input_id) {
		const int layer_id = -1;  // inputs have fake layer ID -1
		AppendTop(param, layer_id, input_id, &available_blobs, &blob_name_to_idx);
	}
	DLOG(INFO)<< "Memory required for data: " << memory_used_ * sizeof(Dtype);
	// For each layer, set up their input and output
	bottom_vecs_.resize(param.layer_size());
	top_vecs_.resize(param.layer_size());
	bottom_id_vecs_.resize(param.layer_size());
	param_id_vecs_.resize(param.layer_size());
	top_id_vecs_.resize(param.layer_size());
	bottom_need_backward_.resize(param.layer_size());
	for (int layer_id = 0; layer_id < param.layer_size(); ++layer_id) {
		const LayerParameter& layer_param = param.layer(layer_id);
		layers_.push_back(
				shared_ptr<Layer<Dtype> >(
						LayerRegistry<Dtype>::CreateLayer(layer_param, replica_id_, net_)));
		layer_names_.push_back(layer_param.name());
		LOG(INFO)<< "Creating Layer " << layer_param.name();
		bool need_backward = false;
		// Figure out this layer's input and output
		for (int bottom_id = 0; bottom_id < layer_param.bottom_size();
				++bottom_id) {
			const int blob_id = AppendBottom(param, layer_id, bottom_id,
					&available_blobs, &blob_name_to_idx);
			// If a blob needs backward, this layer should provide it.
			need_backward |= blob_need_backward_[blob_id];
		}
		int num_top = layer_param.top_size();
		for (int top_id = 0; top_id < num_top; ++top_id) {
			AppendTop(param, layer_id, top_id, &available_blobs, &blob_name_to_idx);
		}
		// If the layer specifies that AutoTopBlobs() -> true and the LayerParameter
		// specified fewer than the required number (as specified by
		// ExactNumTopBlobs() or MinTopBlobs()), allocate them here.
		Layer<Dtype>* layer = layers_[layer_id].get();
		if (layer->AutoTopBlobs()) {
			const int needed_num_top = std::max(layer->MinTopBlobs(),
					layer->ExactNumTopBlobs());
			for (; num_top < needed_num_top; ++num_top) {
				// Add "anonymous" top blobs -- do not modify available_blobs or
				// blob_name_to_idx as we don't want these blobs to be usable as input
				// to other layers.
				AppendTop(param, layer_id, num_top, NULL, NULL);
			}
		}
		// After this layer is connected, set it up.
		LOG(INFO)<< "Setting up " << layer_names_[layer_id];
		layers_[layer_id]->SetUp(bottom_vecs_[layer_id], top_vecs_[layer_id]);
		for (int top_id = 0; top_id < top_vecs_[layer_id].size(); ++top_id) {
			if (blob_loss_weights_.size() <= top_id_vecs_[layer_id][top_id]) {
				blob_loss_weights_.resize(top_id_vecs_[layer_id][top_id] + 1, Dtype(0));
			}
			blob_loss_weights_[top_id_vecs_[layer_id][top_id]] = layer->loss(top_id);
			LOG(INFO)<< "Top shape: " << top_vecs_[layer_id][top_id]->num() << " "
			<< top_vecs_[layer_id][top_id]->channels() << " "
			<< top_vecs_[layer_id][top_id]->height() << " "
			<< top_vecs_[layer_id][top_id]->width() << " ("
			<< top_vecs_[layer_id][top_id]->count() << ")";
			if (layer->loss(top_id)) {
				LOG(INFO)<< "    with loss weight " << layer->loss(top_id);
			}
			memory_used_ += top_vecs_[layer_id][top_id]->count();
		}
		DLOG(INFO)<< "Memory required for data: " << memory_used_ * sizeof(Dtype);
		const int param_size = layer_param.param_size();
		const int num_param_blobs = layers_[layer_id]->blobs().size();
		CHECK_LE(param_size, num_param_blobs)<< "Too many params specified for layer " << layer_param.name();
		ParamSpec default_param_spec;
		for (int param_id = 0; param_id < num_param_blobs; ++param_id) {
			const ParamSpec* param_spec =
					(param_id < param_size) ?
							&layer_param.param(param_id) : &default_param_spec;
			const bool param_need_backward = param_spec->lr_mult() > 0;
			need_backward |= param_need_backward;
			layers_[layer_id]->set_param_propagate_down(param_id,
					param_need_backward);
		}
		for (int param_id = 0; param_id < num_param_blobs; ++param_id) {
			AppendParam(param, layer_id, param_id);
		}
		// Finally, set the backward flag
		layer_need_backward_.push_back(need_backward);
		if (need_backward) {
			for (int top_id = 0; top_id < top_id_vecs_[layer_id].size(); ++top_id) {
				blob_need_backward_[top_id_vecs_[layer_id][top_id]] = true;
			}
		}
	}
	// Go through the net backwards to determine which blobs contribute to the
	// loss.  We can skip backward computation for blobs that don't contribute
	// to the loss.
	set<string> blobs_under_loss;
	for (int layer_id = layers_.size() - 1; layer_id >= 0; --layer_id) {
		bool layer_contributes_loss = false;
		for (int top_id = 0; top_id < top_vecs_[layer_id].size(); ++top_id) {
			const string& blob_name = blob_names_[top_id_vecs_[layer_id][top_id]];
			if (layers_[layer_id]->loss(top_id)
					|| (blobs_under_loss.find(blob_name) != blobs_under_loss.end())) {
				layer_contributes_loss = true;
				break;
			}
		}
		if (!layer_contributes_loss) {
			layer_need_backward_[layer_id] = false;
		}
		if (layer_need_backward_[layer_id]) {
			LOG(INFO)<< layer_names_[layer_id] << " needs backward computation.";
		} else {
			LOG(INFO) << layer_names_[layer_id]
			<< " does not need backward computation.";
		}
		for (int bottom_id = 0; bottom_id < bottom_vecs_[layer_id].size();
				++bottom_id) {
			if (layer_contributes_loss) {
				const string& blob_name =
						blob_names_[bottom_id_vecs_[layer_id][bottom_id]];
				blobs_under_loss.insert(blob_name);
			} else {
				bottom_need_backward_[layer_id][bottom_id] = false;
			}
		}
	}
	// Handle force_backward if needed.
	if (param.force_backward()) {
		for (int layer_id = 0; layer_id < layers_.size(); ++layer_id) {
			layer_need_backward_[layer_id] = true;
			for (int bottom_id = 0;
					bottom_id < bottom_need_backward_[layer_id].size(); ++bottom_id) {
				bottom_need_backward_[layer_id][bottom_id] =
						bottom_need_backward_[layer_id][bottom_id]
								|| layers_[layer_id]->AllowForceBackward(bottom_id);
				blob_need_backward_[bottom_id_vecs_[layer_id][bottom_id]] =
						blob_need_backward_[bottom_id_vecs_[layer_id][bottom_id]]
								|| bottom_need_backward_[layer_id][bottom_id];
			}
			for (int param_id = 0; param_id < layers_[layer_id]->blobs().size();
					++param_id) {
				layers_[layer_id]->set_param_propagate_down(param_id, true);
			}
		}
	}
	// In the end, all remaining blobs are considered output blobs.
	for (set<string>::iterator it = available_blobs.begin();
			it != available_blobs.end(); ++it) {
		LOG(INFO)<< "This network produces output " << *it;
		net_output_blobs_.push_back(blobs_[blob_name_to_idx[*it]].get());
		net_output_blob_indices_.push_back(blob_name_to_idx[*it]);
	}
	for (size_t blob_id = 0; blob_id < blob_names_.size(); ++blob_id) {
		blob_names_index_[blob_names_[blob_id]] = blob_id;
	}
	for (size_t layer_id = 0; layer_id < layer_names_.size(); ++layer_id) {
		layer_names_index_[layer_names_[layer_id]] = layer_id;
	}
	GetLearningRateAndWeightDecay();
	debug_info_ = param.debug_info();
	LOG(INFO)<< "Network initialization done.";
	LOG(INFO)<< "Memory required for data: " << memory_used_ * sizeof(Dtype);
}

template<typename Dtype>
void NetThread<Dtype>::PostInit() {
	Caffe::SetDevice(device_id_);
	InitCuda();

	if (!test_net_) {
		for (int i = 0; i < params_solver_.size(); ++i) {
			params_solver_[i]->PreSolve();
		}
	}
//	solver_->PreSolve();
}

// Helper for Net::Init: add a new input or top blob to the net.  (Inputs have
// layer_id == -1, tops have layer_id >= 0.)
template<typename Dtype>
void NetThread<Dtype>::AppendTop(const NetParameter& param, const int layer_id,
		const int top_id, set<string>* available_blobs,
		map<string, int>* blob_name_to_idx) {
	shared_ptr<LayerParameter> layer_param(
			(layer_id >= 0) ? (new LayerParameter(param.layer(layer_id))) : NULL);
	const string& blob_name =
			layer_param ?
					(layer_param->top_size() > top_id ?
							layer_param->top(top_id) : "(automatic)") :
					param.input(top_id);
	// Check if we are doing in-place computation
	if (blob_name_to_idx && layer_param && layer_param->bottom_size() > top_id
			&& blob_name == layer_param->bottom(top_id)) {
		// In-place computation
		LOG(INFO)<< layer_param->name() << " -> " << blob_name << " (in-place)";
		top_vecs_[layer_id].push_back(blobs_[(*blob_name_to_idx)[blob_name]].get());
		top_id_vecs_[layer_id].push_back((*blob_name_to_idx)[blob_name]);
	} else if (blob_name_to_idx &&
			blob_name_to_idx->find(blob_name) != blob_name_to_idx->end()) {
		// If we are not doing in-place computation but have duplicated blobs,
		// raise an error.
		LOG(FATAL) << "Duplicate blobs produced by multiple sources.";
	} else {
		// Normal output.
		if (layer_param) {
			LOG(INFO) << layer_param->name() << " -> " << blob_name;
		} else {
			LOG(INFO) << "Input " << top_id << " -> " << blob_name;
		}
		shared_ptr<Blob<Dtype> > blob_pointer(new Blob<Dtype>());
		const int blob_id = blobs_.size();
		blobs_.push_back(blob_pointer);
		blob_names_.push_back(blob_name);
		blob_need_backward_.push_back(false);
		if (blob_name_to_idx) {(*blob_name_to_idx)[blob_name] = blob_id;}
		if (layer_id == -1) {
			// Set the (explicitly specified) dimensions of the input blob.
			blob_pointer->Reshape(param.input_dim(top_id * 4),
					param.input_dim(top_id * 4 + 1),
					param.input_dim(top_id * 4 + 2),
					param.input_dim(top_id * 4 + 3));
			net_input_blob_indices_.push_back(blob_id);
			net_input_blobs_.push_back(blob_pointer.get());
		} else {
			top_id_vecs_[layer_id].push_back(blob_id);
			top_vecs_[layer_id].push_back(blob_pointer.get());
		}
	}
	if (available_blobs) {
		available_blobs->insert(blob_name);
	}
}

// Helper for Net::Init: add a new bottom blob to the net.
template<typename Dtype>
int NetThread<Dtype>::AppendBottom(const NetParameter& param,
		const int layer_id, const int bottom_id, set<string>* available_blobs,
		map<string, int>* blob_name_to_idx) {
	const LayerParameter& layer_param = param.layer(layer_id);
	const string& blob_name = layer_param.bottom(bottom_id);
	if (available_blobs->find(blob_name) == available_blobs->end()) {
		LOG(FATAL)<< "Unknown blob input " << blob_name
		<< " (at index " << bottom_id << ") to layer " << layer_id;
	}
	const int blob_id = (*blob_name_to_idx)[blob_name];
	LOG(INFO)<< layer_names_[layer_id] << " <- " << blob_name;
	bottom_vecs_[layer_id].push_back(blobs_[blob_id].get());
	bottom_id_vecs_[layer_id].push_back(blob_id);
	available_blobs->erase(blob_name);
	const bool need_backward = blob_need_backward_[blob_id];
	bottom_need_backward_[layer_id].push_back(need_backward);
	return blob_id;
}

template<typename Dtype>
void NetThread<Dtype>::AppendParam(const NetParameter& param,
		const int layer_id, const int param_id) {
	int num_replicas = net_->GetDeviceIds().size();
	const LayerParameter& layer_param = layers_[layer_id]->layer_param();
	const int param_size = layer_param.param_size();
	string param_name =
			(param_size > param_id) ? layer_param.param(param_id).name() : "";
	if (param_name.size()) {
		param_display_names_.push_back(param_name);
	} else {
		ostringstream param_display_name;
		param_display_name << param_id;
		param_display_names_.push_back(param_display_name.str());
	}
	const int net_param_id = params_.size();

//	layers_[layer_id]->blobs()[param_id]->set_net_thread(this);
//	layers_[layer_id]->blobs()[param_id]->set_param_id(net_param_id);
	params_.push_back(layers_[layer_id]->blobs()[param_id]);
	params_shard_size_.push_back(
			(layers_[layer_id]->blobs()[param_id]->count() + num_replicas - 1)
					/ num_replicas);

	if (!test_net_) {
		shared_ptr<BlobSolver<Dtype> > param_solver;
		param_solver.reset(
				GetBlobSolver(net_->get_solver_param(), net_param_id, this));
		//	(new BlobSolver<Dtype>(net_->get_solver_param(),net_param_id, this));
		params_solver_.push_back(param_solver);
	}

	param_id_vecs_[layer_id].push_back(net_param_id);
	param_layer_indices_.push_back(make_pair(layer_id, param_id));
	if (!param_size || !param_name.size()
			|| (param_name.size()
					&& param_names_index_.find(param_name) == param_names_index_.end())) {
		// This layer "owns" this parameter blob -- it is either anonymous
		// (i.e., not given a param_name) or explicitly given a name that we
		// haven't already seen.
		param_owners_.push_back(-1);
		if (param_size) {
			param_names_index_[param_name] = net_param_id;
		}
	} else {
		// Named param blob with name we've seen before: share params
		const int owner_net_param_id = param_names_index_[param_name];
		param_owners_.push_back(owner_net_param_id);
		const pair<int, int>& owner_index = param_layer_indices_[owner_net_param_id];
		const int owner_layer_id = owner_index.first;
		const int owner_param_id = owner_index.second;
		LOG(INFO)<< "Sharing parameters '" << param_name << "' owned by "
		<< "layer '" << layer_names_[owner_layer_id] << "', param "
		<< "index " << owner_param_id;
		Blob<Dtype>* this_blob = layers_[layer_id]->blobs()[param_id].get();
		Blob<Dtype>* owner_blob =
				layers_[owner_layer_id]->blobs()[owner_param_id].get();
		const int param_size = layer_param.param_size();
		if (param_size > param_id
				&& (layer_param.param(param_id).share_mode()
						== ParamSpec_DimCheckMode_PERMISSIVE)) {
			// Permissive dimension checking -- only check counts are the same.
			CHECK_EQ(this_blob->count(), owner_blob->count())<< "Shared parameter blobs must have the same count.";
		} else {
			// Strict dimension checking -- all dims must be the same.
			CHECK_EQ(this_blob->num(), owner_blob->num())
			<< "Shared parameter blobs must have the same num.";
			CHECK_EQ(this_blob->channels(), owner_blob->channels())
			<< "Shared parameter blobs must have the same channels.";
			CHECK_EQ(this_blob->height(), owner_blob->height())
			<< "Shared parameter blobs must have the same height.";
			CHECK_EQ(this_blob->width(), owner_blob->width())
			<< "Shared parameter blobs must have the same width.";
		}
		layers_[layer_id]->blobs()[param_id]->ShareData(
				*layers_[owner_layer_id]->blobs()[owner_param_id]);
	}
}

template<typename Dtype>
void NetThread<Dtype>::GetLearningRateAndWeightDecay() {
	DLOG(INFO)<< "Collecting Learning Rate and Weight Decay.";
	ParamSpec default_param_spec;
	for (int i = 0; i < layers_.size(); ++i) {
		vector<shared_ptr<Blob<Dtype> > >& layer_blobs = layers_[i]->blobs();
		for (int j = 0; j < layer_blobs.size(); ++j) {
			const ParamSpec* param_spec =
			(layers_[i]->layer_param().param_size() > j) ?
			&layers_[i]->layer_param().param(j) : &default_param_spec;
			params_lr_.push_back(param_spec->lr_mult());
			params_weight_decay_.push_back(param_spec->decay_mult());
		}
	}
}

template<typename Dtype>
Dtype NetThread<Dtype>::ForwardFromTo(int start, int end) {
	DLOG(INFO)<<"NetThread<Dtype>::ForwardFromTo start "<<start<<" end "<<end;
	CHECK_GE(start, 0);
	CHECK_LT(end, layers_.size());
	Dtype loss = 0;
	if (debug_info_) {
		for (int i = 0; i < net_input_blobs_.size(); ++i) {
			InputDebugInfo(i);
		}
	}
	for (int i = start; i <= end; ++i) {
		DLOG(INFO) << "Forwarding " << layer_names_[i];
		layers_[i]->Reshape(bottom_vecs_[i], top_vecs_[i]);
		Dtype layer_loss = layers_[i]->Forward(bottom_vecs_[i], top_vecs_[i]);
		loss += layer_loss;
		if (debug_info_) {
			ForwardDebugInfo(i);
		}
	}
	DLOG(INFO)<<"NetThread<Dtype>::ForwardFromTo completed";
	return loss;
}

template<typename Dtype>
Dtype NetThread<Dtype>::ForwardFrom(int start) {
	return ForwardFromTo(start, layers_.size() - 1);
}

template<typename Dtype>
Dtype NetThread<Dtype>::ForwardTo(int end) {
	return ForwardFromTo(0, end);
}

template<typename Dtype>
const vector<Blob<Dtype>*>& NetThread<Dtype>::ForwardPrefilled(Dtype* loss) {
	DLOG(INFO)<<"NetThread<Dtype>::ForwardPrefilled";
	if (loss != NULL) {
		*loss = ForwardFromTo(0, layers_.size() - 1);
	} else {
		ForwardFromTo(0, layers_.size() - 1);
	}
	DLOG(INFO)<<"NetThread<Dtype>::ForwardPrefilled completed";
	return net_output_blobs_;
}

template<typename Dtype>
const vector<Blob<Dtype>*>& NetThread<Dtype>::Forward(
		const vector<Blob<Dtype>*> &bottom, Dtype* loss) {
	DLOG(INFO)<<"NetThread<Dtype>::Forward device_id "<<this->device_id_
	<<" phase "<<Caffe::get_phase()<<" bottom.size() "<<bottom.size();
	// Copy bottom to internal bottom
	for (int i = 0; i < bottom.size(); ++i) {
		net_input_blobs_[i]->CopyFrom(*bottom[i]);
	}
	return ForwardPrefilled(loss);
}

template<typename Dtype>
string NetThread<Dtype>::Forward(const string& input_blob_protos, Dtype* loss) {
	BlobProtoVector blob_proto_vec;
	if (net_input_blobs_.size()) {
		blob_proto_vec.ParseFromString(input_blob_protos);
		CHECK_EQ(blob_proto_vec.blobs_size(), net_input_blobs_.size())<< "Incorrect input size.";
		for (int i = 0; i < blob_proto_vec.blobs_size(); ++i) {
			net_input_blobs_[i]->FromProto(blob_proto_vec.blobs(i));
		}
	}
	ForwardPrefilled(loss);
	blob_proto_vec.Clear();
	for (int i = 0; i < net_output_blobs_.size(); ++i) {
		net_output_blobs_[i]->ToProto(blob_proto_vec.add_blobs());
	}
	string output;
	blob_proto_vec.SerializeToString(&output);
	return output;
}

template<typename Dtype>
void NetThread<Dtype>::BackwardFromTo(int start, int end) {
	CHECK_GE(end, 0);
	CHECK_LT(start, layers_.size());
	for (int i = start; i >= end; --i) {
		if (layer_need_backward_[i]) {
			layers_[i]->Backward(top_vecs_[i], bottom_need_backward_[i],
					bottom_vecs_[i]);
			if (debug_info_) {
				BackwardDebugInfo(i);
			}
		}
	}
}

template<typename Dtype>
void NetThread<Dtype>::InputDebugInfo(const int input_id) {
	const Blob<Dtype>& blob = *net_input_blobs_[input_id];
	const string& blob_name = blob_names_[net_input_blob_indices_[input_id]];
	const Dtype data_abs_val_mean = blob.asum_data() / blob.count();
	LOG(INFO)<< "    [Forward] "
	<< "Input " << blob_name << " data: " << data_abs_val_mean;
}

template<typename Dtype>
void NetThread<Dtype>::ForwardDebugInfo(const int layer_id) {
	for (int top_id = 0; top_id < top_vecs_[layer_id].size(); ++top_id) {
		const Blob<Dtype>& blob = *top_vecs_[layer_id][top_id];
		const string& blob_name = blob_names_[top_id_vecs_[layer_id][top_id]];
		const Dtype data_abs_val_mean = blob.asum_data() / blob.count();
		LOG(INFO)<< "    [Forward] "
		<< "Layer " << layer_names_[layer_id] << ", top blob " << blob_name
		<< " data: " << data_abs_val_mean;
	}
	for (int param_id = 0; param_id < layers_[layer_id]->blobs().size();
			++param_id) {
		const Blob<Dtype>& blob = *layers_[layer_id]->blobs()[param_id];
		const int net_param_id = param_id_vecs_[layer_id][param_id];
		const string& blob_name = param_display_names_[net_param_id];
		const Dtype data_abs_val_mean = blob.asum_data() / blob.count();
		LOG(INFO) << "    [Forward] "
		<< "Layer " << layer_names_[layer_id] << ", param blob " << blob_name
		<< " data: " << data_abs_val_mean;
	}
}

template<typename Dtype>
void NetThread<Dtype>::BackwardDebugInfo(const int layer_id) {
	const vector<Blob<Dtype>*>& bottom_vec = bottom_vecs_[layer_id];
	for (int bottom_id = 0; bottom_id < bottom_vec.size(); ++bottom_id) {
		if (!bottom_need_backward_[layer_id][bottom_id]) {
			continue;
		}
		const Blob<Dtype>& blob = *bottom_vec[bottom_id];
		const string& blob_name = blob_names_[bottom_id_vecs_[layer_id][bottom_id]];
		const Dtype diff_abs_val_mean = blob.asum_diff() / blob.count();
		LOG(INFO)<< "    [Backward] "
		<< "Layer " << layer_names_[layer_id] << ", bottom blob " << blob_name
		<< " diff: " << diff_abs_val_mean;
	}
	for (int param_id = 0; param_id < layers_[layer_id]->blobs().size();
			++param_id) {
		if (!layers_[layer_id]->param_propagate_down(param_id)) {
			continue;
		}
		const Blob<Dtype>& blob = *layers_[layer_id]->blobs()[param_id];
		const Dtype diff_abs_val_mean = blob.asum_diff() / blob.count();
		LOG(INFO)<< "    [Backward] "
		<< "Layer " << layer_names_[layer_id] << ", param blob " << param_id
		<< " diff: " << diff_abs_val_mean;
	}
}

template<typename Dtype>
void NetThread<Dtype>::UpdateDebugInfo(const int param_id) {
	const Blob<Dtype>& blob = *params_[param_id];
	const int param_owner = param_owners_[param_id];
	const string& layer_name = layer_names_[param_layer_indices_[param_id].first];
	const string& param_display_name = param_display_names_[param_id];
	const Dtype diff_abs_val_mean = blob.asum_diff() / blob.count();
	if (param_owner < 0) {
		const Dtype data_abs_val_mean = blob.asum_data() / blob.count();
		LOG(INFO)<< "    [Update] Layer " << layer_name
		<< ", param " << param_display_name
		<< " data: " << data_abs_val_mean << "; diff: " << diff_abs_val_mean;
	} else {
		const string& owner_layer_name =
		layer_names_[param_layer_indices_[param_owner].first];
		LOG(INFO) << "    [Update] Layer " << layer_name
		<< ", param blob " << param_display_name
		<< " (owned by layer " << owner_layer_name << ", "
		<< "param " << param_display_names_[param_owners_[param_id]] << ")"
		<< " diff: " << diff_abs_val_mean;
	}
}

template<typename Dtype>
void NetThread<Dtype>::ShareTrainedLayersWith(const NetThread<Dtype>* other) {
	int num_source_layers = other->layers().size();
	for (int i = 0; i < num_source_layers; ++i) {
		Layer<Dtype>* source_layer = other->layers()[i].get();
		const string& source_layer_name = other->layer_names()[i];
		int target_layer_id = 0;
		while (target_layer_id != layer_names_.size()
				&& layer_names_[target_layer_id] != source_layer_name) {
			++target_layer_id;
		}
		if (target_layer_id == layer_names_.size()) {
			DLOG(INFO)<< "Ignoring source layer " << source_layer_name;
			continue;
		}
		DLOG(INFO)<< "Copying source layer " << source_layer_name;
		vector<shared_ptr<Blob<Dtype> > >& target_blobs =
				layers_[target_layer_id]->blobs();
		CHECK_EQ(target_blobs.size(), source_layer->blobs().size())<< "Incompatible number of blobs for layer " << source_layer_name;
		for (int j = 0; j < target_blobs.size(); ++j) {
			Blob<Dtype>* source_blob = source_layer->blobs()[j].get();
			CHECK_EQ(target_blobs[j]->num(), source_blob->num());
			CHECK_EQ(target_blobs[j]->channels(), source_blob->channels());
			CHECK_EQ(target_blobs[j]->height(), source_blob->height());
			CHECK_EQ(target_blobs[j]->width(), source_blob->width());
			target_blobs[j]->ShareData(*source_blob);
		}
	}
}

template<typename Dtype>
void NetThread<Dtype>::BackwardFrom(int start) {
	BackwardFromTo(start, 0);
}

template<typename Dtype>
void NetThread<Dtype>::BackwardTo(int end) {
	BackwardFromTo(layers_.size() - 1, end);
}

template<typename Dtype>
void NetThread<Dtype>::Backward() {
	BackwardFromTo(layers_.size() - 1, 0);
	if (debug_info_) {
		Dtype asum_data = 0, asum_diff = 0, sumsq_data = 0, sumsq_diff = 0;
		for (int i = 0; i < params_.size(); ++i) {
			if (param_owners_[i] >= 0) {
				continue;
			}
			asum_data += params_[i]->asum_data();
			asum_diff += params_[i]->asum_diff();
			sumsq_data += params_[i]->sumsq_data();
			sumsq_diff += params_[i]->sumsq_diff();
		}
		const Dtype l2norm_data = std::sqrt(sumsq_data);
		const Dtype l2norm_diff = std::sqrt(sumsq_diff);
		LOG(ERROR)<< "    [Backward] All net params (data, diff): "
		<< "L1 norm = (" << asum_data << ", " << asum_diff << "); "
		<< "L2 norm = (" << l2norm_data << ", " << l2norm_diff << ")";
	}
}

template<typename Dtype>
void NetThread<Dtype>::Reshape() {
	for (int i = 0; i < layers_.size(); ++i) {
		layers_[i]->Reshape(bottom_vecs_[i], top_vecs_[i]);
	}
}

template<typename Dtype>
void NetThread<Dtype>::CopyTrainedLayersFrom(const NetParameter& param) {
	int num_source_layers = param.layer_size();
	for (int i = 0; i < num_source_layers; ++i) {
		const LayerParameter& source_layer = param.layer(i);
		const string& source_layer_name = source_layer.name();
		int target_layer_id = 0;
		while (target_layer_id != layer_names_.size()
				&& layer_names_[target_layer_id] != source_layer_name) {
			++target_layer_id;
		}
		if (target_layer_id == layer_names_.size()) {
			DLOG(INFO)<< "Ignoring source layer " << source_layer_name;
			continue;
		}
		DLOG(INFO)<< "Copying source layer " << source_layer_name;
		vector<shared_ptr<Blob<Dtype> > >& target_blobs =
				layers_[target_layer_id]->blobs();
		CHECK_EQ(target_blobs.size(), source_layer.blobs_size())<< "Incompatible number of blobs for layer " << source_layer_name;
		for (int j = 0; j < target_blobs.size(); ++j) {
			CHECK_EQ(target_blobs[j]->num(), source_layer.blobs(j).num());
			CHECK_EQ(target_blobs[j]->channels(), source_layer.blobs(j).channels());
			CHECK_EQ(target_blobs[j]->height(), source_layer.blobs(j).height());
			CHECK_EQ(target_blobs[j]->width(), source_layer.blobs(j).width());
			target_blobs[j]->FromProto(source_layer.blobs(j));
		}
	}
}

template<typename Dtype>
void NetThread<Dtype>::CopyTrainedLayersFrom(const string trained_filename) {
	NetParameter param;
	ReadNetParamsFromBinaryFileOrDie(trained_filename, &param);
	CopyTrainedLayersFrom(param);
}

template<typename Dtype>
void NetThread<Dtype>::ToProto(NetParameter* param, bool write_diff) const {
	param->Clear();
	param->set_name(name_);
	// Add bottom and top
	for (int i = 0; i < net_input_blob_indices_.size(); ++i) {
		param->add_input(blob_names_[net_input_blob_indices_[i]]);
	}
	DLOG(INFO)<< "Serializing " << layers_.size() << " layers";
	for (int i = 0; i < layers_.size(); ++i) {
		LayerParameter* layer_param = param->add_layer();
		for (int j = 0; j < bottom_id_vecs_[i].size(); ++j) {
			layer_param->add_bottom(blob_names_[bottom_id_vecs_[i][j]]);
		}
		for (int j = 0; j < top_id_vecs_[i].size(); ++j) {
			layer_param->add_top(blob_names_[top_id_vecs_[i][j]]);
		}
		layers_[i]->ToProto(layer_param, write_diff);
	}
}

template<typename Dtype>
void NetThread<Dtype>::AggregateGradient(){
	for (int param_id = 0; param_id < params_solver_.size(); ++param_id) {
		params_solver_[param_id]->AggregateGradient();
	}
}


template<typename Dtype>
void NetThread<Dtype>::ComputeUpdateValue() {
//	solver_->ComputeUpdateValue();
//	Caffe::SyncDevice();
	nvtxMarkA("NetThread<Dtype>::ComputeUpdateValue");
	for (int param_id = 0; param_id < params_solver_.size(); ++param_id) {
		params_solver_[param_id]->ComputeUpdateValue();
	}
//	Caffe::SyncDevice();
}

template<typename Dtype>
void NetThread<Dtype>::Update() {
	nvtxMarkA("NetThread<Dtype>::Update() m0");

//	Caffe::SyncDevice();
	// First, accumulate the diffs of any shared parameters into their owner's
	// diff. (Assumes that the learning rate, weight decay, etc. have already been
	// accounted for in the current diff.)
	for (int i = 0; i < params_.size(); ++i) {
		if (param_owners_[i] < 0) {
			continue;
		}
		if (debug_info_) {
			UpdateDebugInfo(i);
		}
		const int count = params_[i]->count();
		const Dtype* this_diff;
		Dtype* owner_diff;
		switch (Caffe::mode()) {
		case Caffe::CPU:
			this_diff = params_[i]->cpu_diff();
			owner_diff = params_[param_owners_[i]]->mutable_cpu_diff();
			caffe_add(count, this_diff, owner_diff, owner_diff);
			break;
#ifndef CPU_ONLY
		case Caffe::GPU:
			this_diff = params_[i]->gpu_diff();
			owner_diff = params_[param_owners_[i]]->mutable_gpu_diff();
			caffe_gpu_add(count, this_diff, owner_diff, owner_diff);
			break;
#else
			NO_GPU;
#endif
		default:
			LOG(FATAL)<< "Unknown caffe mode: " << Caffe::mode();
		}
	}
	// Now, update the owned parameters.
	for (int i = 0; i < params_.size(); ++i) {
		if (param_owners_[i] >= 0) {continue;}
		if (debug_info_) {UpdateDebugInfo(i);}
		params_[i]->Update();
	}
	nvtxMarkA("NetThread<Dtype>::Update() m1");
}

template<typename Dtype>
bool NetThread<Dtype>::has_blob(const string& blob_name) const {
	return blob_names_index_.find(blob_name) != blob_names_index_.end();
}

template<typename Dtype>
const shared_ptr<Blob<Dtype> > NetThread<Dtype>::blob_by_name(
		const string& blob_name) const {
	shared_ptr<Blob<Dtype> > blob_ptr;
	if (has_blob(blob_name)) {
		blob_ptr = blobs_[blob_names_index_.find(blob_name)->second];
	} else {
		blob_ptr.reset((Blob<Dtype>*) (NULL));
		LOG(WARNING)<< "Unknown blob name " << blob_name;
	}
	return blob_ptr;
}

template<typename Dtype>
bool NetThread<Dtype>::has_layer(const string& layer_name) const {
	return layer_names_index_.find(layer_name) != layer_names_index_.end();
}

template<typename Dtype>
const shared_ptr<Layer<Dtype> > NetThread<Dtype>::layer_by_name(
		const string& layer_name) const {
	shared_ptr<Layer<Dtype> > layer_ptr;
	if (has_layer(layer_name)) {
		layer_ptr = layers_[layer_names_index_.find(layer_name)->second];
	} else {
		layer_ptr.reset((Layer<Dtype>*) (NULL));
		LOG(WARNING)<< "Unknown layer name " << layer_name;
	}
	return layer_ptr;
}

template<typename Dtype>
shared_ptr<Blob<Dtype> > NetThread<Dtype>::GetShardGPUOnly(
		const shared_ptr<Blob<Dtype> > &p, int param_id, int replica_id) {
	int n = p->count();
	shared_ptr<Blob<Dtype> > p2 = p->Reshaped(n, 1, 1, 1);
	int start_num = params_shard_size_[param_id] * replica_id;
	int end_num = std::min(start_num + params_shard_size_[param_id], n);
//	LOG(INFO)<<"NetThread<Dtype>::GetShard start "<<start_num<<" end_num "<<end_num;
	return p2->SliceNumGPUOnly(start_num, end_num);;
}

template<typename Dtype>
shared_ptr<Blob<Dtype> > NetThread<Dtype>::GetShardGPUOnly(int param_id,
		int replica_id) {
	params_mutex_.lock_shared();
	shared_ptr<Blob<Dtype> > p = params_[param_id];
	int n = p->count();
	shared_ptr<Blob<Dtype> > p2 = p->ReshapedGPUOnly(n, 1, 1, 1);
	int start_num = params_shard_size_[param_id] * replica_id;
	int end_num = std::min(start_num + params_shard_size_[param_id], n);
//	LOG(INFO)<<"NetThread<Dtype>::GetShard start "<<start_num<<" end_num "<<end_num;
	shared_ptr<Blob<Dtype> > shard = p2->SliceNumGPUOnly(start_num, end_num);
	params_mutex_.unlock_shared();
	return shard;
}

template<typename Dtype>
void NetThread<Dtype>::CreateNetThread() {
	CHECK(StartInternalThread()) << "Net thread execution failed";
}

template<typename Dtype>
void NetThread<Dtype>::JoinNetThread() {
	CHECK(WaitForInternalThreadToExit()) << "Net thread joining failed";
}

template<typename Dtype>
void NetThread<Dtype>::InternalThreadEntry() {
	char message_str[1024];
	sprintf(message_str, "Net thread device id %d", device_id_);
	nvtxNameOsThread(pthread_self(),message_str);

//	LOG(INFO)<<"NetThread<Dtype>::InternalThreadEntry device_id_ "<<device_id_;
	Caffe::SetDevice(device_id_);
	srand(time(0));

	switch (work_message_.getType()) {
	case NO_WORK:
		break;
	case AGGREGATE_GRADIENT:
		AggregateGradient();
		break;
	case COMPUTE_UPDATE_VALUE:
		ComputeUpdateValue();
		break;
	case UPDATE_WEIGHTS:
		Update();
		break;
	case FORWARD:
		Forward(bottom_, &loss_);
		break;
	case FORWARD_PREFILLED:
		ForwardPrefilled(&loss_);
		break;
	case FORWARD_INPUT_BLOB_PROTOS:
		Forward(input_blob_protos_, &loss_);
		break;
	case FORWARD_BACKWARD:
		Forward(bottom_, &loss_);
		Backward();
		break;
	default:
		LOG(WARNING)<<"Unknown working message type";
		break;
	}
}

INSTANTIATE_CLASS(NetThread);

}  // namespace caffe
