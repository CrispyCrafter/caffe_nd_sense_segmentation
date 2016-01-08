#include <boost/thread.hpp>
#include <map>
#include <string>
#include <vector>
#include "caffe/util/io.hpp"
#include "caffe/common.hpp"
#include "caffe/patch_sampler.hpp"
#include "caffe/proto/caffe.pb.h"

namespace caffe {


//template class PatchSampler<float>;
//template class PatchSampler<double>;

using boost::weak_ptr;

template <typename Dtype>
map<const string, weak_ptr<Runner<Dtype> > > PatchSampler<Dtype>::runners_;

static boost::mutex runners_mutex_;
template <typename Dtype>
PatchSampler<Dtype>::PatchSampler(const LayerParameter& param)
    :param_(param),
    queue_pair_(new QueuePair_Batch<Dtype>(param)),
    d_provider_(Make_data_provider_instance<Dtype>(param.data_provider_param())),
    patch_coord_finder_(new PatchCoordFinder<Dtype>(param)),
    sample_selector_(new SampleSelector<Dtype>(param)),
    data_transformer_nd(new DataTransformerND<Dtype>(param.transform_nd_param()))
{
  // Get or create a body
  boost::mutex::scoped_lock lock(runners_mutex_);
  string key = source_key(param);
  LOG(INFO)<<"patch sample key = "<<key;
  weak_ptr<Runner<Dtype> >& weak = runners_[key];
  runner_ = weak.lock();
  if (!runner_) {
    runner_.reset(new Runner<Dtype>(param,*this));
    runners_[key] = weak_ptr<Runner<Dtype> >(runner_);
    LOG(INFO)<<("reset runner ...");
  }
  runner_->new_queue_pairs_.push(queue_pair_);
  LOG(INFO)<<("runner setup done ...");

  patches_per_data_batch_ = param_.patch_sampler_param().patches_per_data_batch();
  data_transformer_nd->InitRand();
  const unsigned int prefetch_rng_seed = caffe_rng_rand();
  prefetch_rng_.reset(new Caffe::RNG(prefetch_rng_seed));
  patch_count_ =0;
  int data_patch_axims  = param_.patch_sampler_param().data_patch_shape().dim_size();
  int label_patch_axims = param_.patch_sampler_param().label_patch_shape().dim_size();
  CHECK_EQ(data_patch_axims,label_patch_axims);
  for(int i =0;i<data_patch_axims;++i){
    patch_data_shape_.push_back(param_.patch_sampler_param().data_patch_shape().dim(i));
    patch_label_shape_.push_back(param_.patch_sampler_param().label_patch_shape().dim(i));
    CHECK_GE(patch_data_shape_[i],patch_label_shape_[i]);
  }

}

template <typename Dtype>
PatchSampler<Dtype>::~PatchSampler() {
  string key = source_key(runner_->param_);
  runner_.reset();
  boost::mutex::scoped_lock lock(runners_mutex_);
  if (runners_[key].expired()) {
    runners_.erase(key);
  }
}

template <typename Dtype>
unsigned int PatchSampler<Dtype>::PrefetchRand(){
  CHECK(prefetch_rng_);
  caffe::rng_t* prefetch_rng =
      static_cast<caffe::rng_t*>(prefetch_rng_->generator());
  return (*prefetch_rng)();
}

template <typename Dtype>
vector<int> PatchSampler<Dtype>::patch_data_shape(){
  if (dest_data_shape_.size()==0){
    //LOG(INFO)<<"start readone";
    ReadOnePatch(queue_pair_.get());
    //LOG(INFO)<<"finished readone";
  }
//  LOG(INFO)<<"dest_data_shape_ size = "<<dest_data_shape_.size();
  return dest_data_shape_;
}
template <typename Dtype>
vector<int> PatchSampler<Dtype>::patch_label_shape(){
  if (dest_label_shape_.size()==0){
    ReadOnePatch(queue_pair_.get());
  }
  return dest_label_shape_;
}

template <typename Dtype>
void PatchSampler<Dtype>::ReadOnePatch(QueuePair_Batch<Dtype>* qb ){
  // load new data to memomry pool
// LOG(INFO)<<"patch read count "<<patch_count_;
 count_m_mutex_.lock();
  if(patch_count_%patches_per_data_batch_ ==0)
  {

      LOG(INFO)<<"loading batch patch_count = "<<patch_count_;
      d_provider_->Load_next_batch();


  }
  patch_count_++;




  int data_idx=PrefetchRand()% d_provider_->get_current_batch_size();
  const Batch_data<Dtype> source_data_label=d_provider_->getOneData(data_idx);

  //LOG(INFO)<<()



  Batch_data<Dtype>* patch_data_label = qb->free_.pop();
  //LOG(INFO)<< "readone from provider";
  // take input patch_data then warp a patch and put it to patch_data;
  // the function that address the probability of selecting classe need to be addressed
   //CropCenterInfo<Dtype>& PeekCropCenterPoint(Blob<Dtype>* input_blob);

  //   const CropCenterInfo<Dtype>& PeekCropCenterPoint(Blob<Dtype>* input_blob);
  //const CropCenterInfo<Dtype>& crp_cent_info=data_transformer_nd->PeekCropCenterPoint(source_data_label.label_.get());

  vector<int> s_data_shape=source_data_label.label_.get()->shape();
  CHECK_GE(s_data_shape.size(),3);
  vector<int> real_data_shape(s_data_shape.begin()+2,s_data_shape.end());
  patch_coord_finder_->SetInputShape(real_data_shape);

  vector<int> randPt ;
  Dtype pt_label_value ;
  do{
      randPt                = patch_coord_finder_->GetRandomPatchCenterCoord();
      pt_label_value        = data_transformer_nd->ReadOnePoint(source_data_label.label_.get(), randPt);
  }while (!sample_selector_->AcceptGivenLabel((int)pt_label_value));

  //LOG(INFO)<<"selected Label = "<<pt_label_value;


  vector<int> data_offset     = patch_coord_finder_->GetDataOffset();
  vector<int> label_offset    = patch_coord_finder_->GetLabelOffset();
  // for(int i=0;i<randPt.size();++i){
  //   LOG(INFO)<<"randPt "<<"["<<i<<"] = "<<  randPt[i];
  //   LOG(INFO)<<"label_offset  "<<"["<<i<<"] = "<<  label_offset[i];
  //   LOG(INFO)<<"data_offset  "<<"["<<i<<"] = "<<  data_offset[i];
  // }

  //randPt.insert(s_data_shape.begin(),s_data_shape.begin()+2);
  //CropCenterInfo<Dtype> crp_cent_info=data_transformer_nd->PeekCropCenterPoint(source_data_label.label_.get());
  //LOG(INFO)<<"nd_off num_aix after return  ="<<crp_cent_info.nd_off.size();


  //Blob<Dtype> &trans_data_blob = *patch_data_label->data_;
  //Blob<Dtype> &trans_label_blob =*patch_data_label->label_;


//Blob<Dtype> trans_data_blob,trans_label_blob;


  //LOG(INFO)<<"start transform";
//  data_transformer_nd->Transform(source_data_label.data_.get(), &trans_data_blob, crp_cent_info.nd_off);

  // data_transformer_nd->Transform(source_data_label.data_.get(),
  //                                   &trans_data_blob,
  //                                   data_offset,
  //                                   patch_data_shape_);
  // data_transformer_nd->Transform(source_data_label.label_.get(),
  //                                   &trans_label_blob,
  //                                   label_offset,
  //                                   patch_label_shape_);

                                    data_transformer_nd->Transform(source_data_label.data_.get(),
                                                                      patch_data_label->data_.get(),
                                                                      data_offset,
                                                                      patch_data_shape_);
                                    data_transformer_nd->Transform(source_data_label.label_.get(),
                                                                      patch_data_label->label_.get(),
                                                                      label_offset,
                                                                      patch_label_shape_);
  count_m_mutex_.unlock();
 //LOG(INFO)<<"end transform";
  const vector<int>& source_data_shape =source_data_label.data_->shape();
  const vector<int>& source_label_shape =source_data_label.label_->shape();




  int new_label=(int)  patch_data_label->label_.get()->cpu_data()[0];
  if(patch_data_label->label_.get()->count()==1)
  {CHECK_EQ(new_label,pt_label_value);}



  int d_dims  =source_data_shape.size();
  int l_dims  =source_label_shape.size();
  //LOG(INFO)<<"data size = "<<d_dims;
  //LOG(INFO)<<"label in blob = "<<patch_data_label->label_.get()->cpu_data()[0];

  int d_num   =source_data_shape[0];
  int l_num   =source_label_shape[0];
  CHECK_EQ(d_dims,l_dims);
  CHECK_EQ(d_num,1);
  CHECK_EQ(l_num,1);
  for(int i=2;i<d_dims;i++){
    int d_dim=source_data_shape[i];
    int l_dim=source_label_shape[i];
    CHECK_EQ(d_dim,l_dim);
  }






  //vector<int>& source_shape =patch_data->data.shape();
  //vector<int> dest_label_shape;

  dest_label_shape_.clear();
  dest_data_shape_.clear();
  // for(int i=1;i< d_dims;i++){
  //   dest_label_shape_.push_back(1);
  // }

//****===================================================***//
// the data dest_label_shape_[0] and dest_data_shape_[0]
// are  suppose to be 1 as each patch is just one data from the source data valum//
//====================================================////
 //LOG(INFO)<<"copy blob from trans_data_blob";

//  patch_data_label->data_->CopyFrom(trans_data_blob,false,true);
//  patch_data_label->label_->CopyFrom(trans_label_blob,false,true);


  //patch_data_label->label_->Reshape(dest_label_shape_);

  //patch_data_label->label_->mutable_cpu_data()[0]=crp_cent_info.value;
  //patch_data_label->label_->mutable_cpu_data()[0]=pt_label_value;
  qb->full_.push(patch_data_label);
  //LOG(INFO)<<"put q back to quque";
  // setting patch data and label shape;
  dest_label_shape_=patch_data_label->label_->shape();
  dest_data_shape_=patch_data_label->data_->shape();


}
//
template <typename Dtype>
QueuePair_Batch<Dtype>::QueuePair_Batch(const LayerParameter& param) {
  // Initialize the free queue with requested number of blobs
//  Batch_dat
  int batch_size = param.patch_sampler_param().batch_size();
//  int patch_dims = param.patch_sampler_param().patch_shape().dim_size();
  int data_patch_dims = param.patch_sampler_param().data_patch_shape().dim_size();
  int label_patch_dims = param.patch_sampler_param().label_patch_shape().dim_size();
  CHECK_GE(data_patch_dims,1);
  CHECK_EQ(data_patch_dims,label_patch_dims);
  vector<int> data_patch_shape, label_patch_shape;
  data_patch_shape.push_back(batch_size);
  label_patch_shape.push_back(batch_size);

  data_patch_shape.push_back(1);  // for channels
  label_patch_shape.push_back(1);
  //LOG(INFO)<<"batch_size = " << label_shape[0];
  for(int i=0;i<data_patch_dims;i++){
   //  int dim =param.patch_sampler_param().patch_shape().dim(i);
   int dim_d=param.patch_sampler_param().data_patch_shape().dim(i);
   int dim_l=param.patch_sampler_param().label_patch_shape().dim(i);
    data_patch_shape.push_back(dim_d);
    label_patch_shape.push_back(dim_l);
  //  LOG(INFO)<<"batch_size = " << patch_shape[i+2] <<" crop_dim = "<<dim;
  }

    // for(int i=1;i<patch_dims;i++){
    //    label_shape.push_back(1);
    //  }


  // bool output_label =param.patch_sampler_param().has_label_shape();
  //bool output_label =true;



  // if(output_label){
  //   int label_dim = param.patch_sampler_param().label_shape().dim_size();
  //   for(int i=1;i<label_dim;i++){
  //      label_shape.push_back(label_dim);
  //    }
  // }



  for (int i = 0; i < batch_size*2; ++i) {
    //Batch_data<Dtype>* b_d =new
  //  Batch_data<Dtype>* b_data =new Batch_data<Dtype>();
    free_.push(new Batch_data<Dtype>);
    //Batch_data<Dtype>& b_data = *( free_.peek());
  //  Batch_data<Dtype>& b_data = *( free_.pop());

    //  b_data.data_.reset(new Blob<Dtype>);
    //  b_data.label_.reset(new Blob<Dtype>);
  //  LOG(INFO)<<"init queuePair_batch... data reshape dim =" <<patch_shape.size();
    //b_data.data_->Reshape(data_patch_shape);

    //b_data->data_->Reshape(data_patch_shape);
      //LOG(INFO)<<"init queuePair_batch... label reshape dim =" <<patch_shape.size();
    //if(output_label){
      //b_data.label_->Reshape(label_patch_shape);
    //  b_data->label_->Reshape(label_patch_shape);
    //}
    //  free_.push(b_data);
  }
}

template <typename Dtype>
QueuePair_Batch<Dtype>::~QueuePair_Batch() {
  Batch_data<Dtype>* bd;
  while (free_.try_pop(&bd)) {
    delete bd;
  }
  while (full_.try_pop(&bd)) {
    delete bd;
  }
}

//
template <typename Dtype>
Runner<Dtype>::Runner(const LayerParameter& param, PatchSampler<Dtype>& p_sampler)
    : param_(param),
      p_sampler_(p_sampler),
      new_queue_pairs_() {

        StartInternalThread();
}


template <typename Dtype>
Runner<Dtype>::~Runner() {
  StopInternalThread();
}

template <typename Dtype>
void Runner<Dtype>::InternalThreadEntry() {

      vector<shared_ptr<QueuePair_Batch<Dtype> > > qps;
      try {
          int solver_count = param_.phase() == TRAIN ? Caffe::solver_count() : 1;

          // To ensure deterministic runs, only start running once all solvers
          // are ready. But solvers need to peek on one item during initialization,
          // so read one item, then wait for the next solver.
          LOG(INFO)<<"solver_count  = "<<solver_count << "  size of queue pairs = "<<new_queue_pairs_.size();
          for (int i = 0; i < solver_count; ++i) {
            //LOG(INFO)<<"new_queue_pairs_.pop() = "<<i;
            shared_ptr<QueuePair_Batch<Dtype> > qp(new_queue_pairs_.pop());
            //LOG(INFO)<<"size of queue  is now = " <<new_queue_pairs_.size();
            qps.push_back(qp);
          }
          while (!must_stop()) {
            for (int i = 0; i < solver_count; ++i) {
              p_sampler_.ReadOnePatch(qps[i].get());

            }
          }
        } catch (boost::thread_interrupted&) {
          // Interrupted exception is expected on shutdown
        }
   }


  template <typename Dtype>
  PatchCoordFinder<Dtype>::PatchCoordFinder(const LayerParameter& param)
  :param_(param),has_label_shape_(false),input_shape_(1,0)
  {
    CHECK(param_.patch_sampler_param().has_data_patch_shape())
    <<"patch data shape mush be given in patch_sampler_param";
    has_label_shape_=param_.patch_sampler_param().has_label_patch_shape();
    int dim_data_size = param_.patch_sampler_param().data_patch_shape().dim_size();
    if(has_label_shape_){
      int dim_label_size = param_.patch_sampler_param().label_patch_shape().dim_size();
      CHECK_EQ(dim_data_size, dim_label_size)
      <<"patch dimention axis of data and label must equal";
      }

    for (int i =0 ;i <dim_data_size;++i){
      data_shape_.push_back(param_.patch_sampler_param().data_patch_shape().dim(i));
      if(has_label_shape_){
        label_shape_.push_back(param_.patch_sampler_param().label_patch_shape().dim(i));
      }
    }

    InitRand();


    int data_patch_dims  =data_shape_.size();
    data_shape_offset_.resize(data_patch_dims,0);
    label_shape_center_.resize(data_patch_dims,0);
    if(has_label_shape_)  label_shape_offset_.resize(data_patch_dims,0);

  }

  template <typename Dtype>
   void PatchCoordFinder<Dtype>::SetInputShape(vector<int> input_shape){
          input_shape_=input_shape;
   }

   template <typename Dtype>
    vector<int> PatchCoordFinder<Dtype>:: GetDataOffset(){
     return data_shape_offset_;
   }

   template <typename Dtype>
    vector<int> PatchCoordFinder<Dtype>:: GetLabelOffset(){
     return label_shape_offset_;
   }

   template <typename Dtype>
   vector<int>  PatchCoordFinder<Dtype>::GetRandomPatchCenterCoord(){
     CHECK_GT(input_shape_[0],0)<< "input data shape has not been set, using SetInputShape member function";

    // vector<int> center_point(data_patch_dims,0);


        for(int i=0;i<data_shape_offset_.size();i++){
            int min_point    =    label_shape_[i]/2;
            int max_point    =    input_shape_[i]-label_shape_[i]/2-1;
            label_shape_center_[i]  =    Rand(max_point-min_point)+min_point;
            label_shape_offset_[i]  =     label_shape_center_[i] - min_point;
            data_shape_offset_[i]   =     label_shape_center_[i] - data_shape_[i]/2;
       }
       return label_shape_center_;
   }

  template <typename Dtype>
  void PatchCoordFinder<Dtype>::InitRand(){
     const bool needs_rand = param_.patch_sampler_param().has_data_patch_shape();
    if (needs_rand) {
      const unsigned int rng_seed = caffe_rng_rand();
      rng_.reset(new Caffe::RNG(rng_seed));
    } else {
      rng_.reset();
    }
  }

  template <typename Dtype>
  int PatchCoordFinder<Dtype>::Rand(int n) {
    CHECK(rng_);
    CHECK_GT(n, 0);
    caffe::rng_t* rng =
        static_cast<caffe::rng_t*>(rng_->generator());
    return ((*rng)() % n);
  }



  INSTANTIATE_CLASS(Runner);
  INSTANTIATE_CLASS(QueuePair_Batch);
  INSTANTIATE_CLASS(PatchSampler);
  INSTANTIATE_CLASS(PatchCoordFinder);

 }
 // namespace caffe
