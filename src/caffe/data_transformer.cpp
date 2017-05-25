#include <opencv2/core/core.hpp>

#include <string>
#include <vector>
#include <opencv2/imgproc/imgproc.hpp>

#include "caffe/data_transformer.hpp"
#include "caffe/util/io.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/util/rng.hpp"

namespace caffe {

template<typename Dtype>
DataTransformer<Dtype>::DataTransformer(const TransformationParameter& param,
    Phase phase)
    : param_(param), phase_(phase) {
  // check if we want to use mean_file
  if (param_.has_mean_file()) {
    CHECK_EQ(param_.mean_value_size(), 0) <<
      "Cannot specify mean_file and mean_value at the same time";
    const string& mean_file = param.mean_file();
    LOG(INFO) << "Loading mean file from: " << mean_file;
    BlobProto blob_proto;
    ReadProtoFromBinaryFileOrDie(mean_file.c_str(), &blob_proto);
    data_mean_.FromProto(blob_proto);
  }
  // check if we want to use mean_value
  if (param_.mean_value_size() > 0) {
    CHECK(param_.has_mean_file() == false) <<
      "Cannot specify mean_file and mean_value at the same time";
    for (int c = 0; c < param_.mean_value_size(); ++c) {
      mean_values_.push_back(param_.mean_value(c));
    }
  }

  //load multiscale info
  max_distort_ = param_.max_distort();
  custom_scale_ratios_.clear();
  for (int i = 0; i < param_.scale_ratios_size(); ++i){
    custom_scale_ratios_.push_back(param_.scale_ratios(i));
  }
  org_size_proc_ = param.original_image();
}



/** @build fixed crop offsets for random selection
 */
void fillFixOffset(int datum_height, int datum_width, int crop_height, int crop_width,
                   bool more_crop,
                   vector<pair<int , int> >& offsets){
  int height_off = (datum_height - crop_height)/4;
  int width_off = (datum_width - crop_width)/4;

  offsets.clear();
  offsets.push_back(pair<int, int>(0, 0)); //upper left
  offsets.push_back(pair<int, int>(0, 4 * width_off)); //upper right
  offsets.push_back(pair<int, int>(4 * height_off, 0)); //lower left
  offsets.push_back(pair<int, int>(4 * height_off, 4 *width_off)); //lower right
  offsets.push_back(pair<int, int>(2 * height_off, 2 * width_off)); //center

  //will be used when more_fix_crop is set to true
  if (more_crop) {
    offsets.push_back(pair<int, int>(0, 2 * width_off)); //top center
    offsets.push_back(pair<int, int>(4 * height_off, 2 * width_off)); //bottom center
    offsets.push_back(pair<int, int>(2 * height_off, 0)); //left center
    offsets.push_back(pair<int, int>(2 * height_off, 4 * width_off)); //right center

    offsets.push_back(pair<int, int>(1 * height_off, 1 * width_off)); //upper left quarter
    offsets.push_back(pair<int, int>(1 * height_off, 3 * width_off)); //upper right quarter
    offsets.push_back(pair<int, int>(3 * height_off, 1 * width_off)); //lower left quarter
    offsets.push_back(pair<int, int>(3 * height_off, 3 * width_off)); //lower right quarter
  }
}

float _scale_rates[] = {1.0, .875, .75, .66};
vector<float> default_scale_rates(_scale_rates, _scale_rates + sizeof(_scale_rates)/ sizeof(_scale_rates[0]) );

/**
 * @generate crop size when multi-scale cropping is requested
 */
void fillCropSize(int input_height, int input_width,
                 int net_input_height, int net_input_width,
                 vector<pair<int, int> >& crop_sizes,
                 int max_distort, vector<float>& custom_scale_ratios){
    crop_sizes.clear();

    vector<float>& scale_rates = (custom_scale_ratios.size() > 0)?custom_scale_ratios:default_scale_rates;
    int base_size = std::min(input_height, input_width);
    for (int h = 0; h < scale_rates.size(); ++h){
      int crop_h = int(base_size * scale_rates[h]);
      crop_h = (abs(crop_h - net_input_height) < 3)?net_input_height:crop_h;
      for (int w = 0; w < scale_rates.size(); ++w){
        int crop_w = int(base_size * scale_rates[w]);
        crop_w = (abs(crop_w - net_input_width) < 3)?net_input_width:crop_w;

        //append this cropping size into the list
        if (abs(h-w)<=max_distort) {
          crop_sizes.push_back(pair<int, int>(crop_h, crop_w));
        }
      }
    }
}

/**
 * @generate crop size and offset when process original images
 */
void sampleRandomCropSize(int img_height, int img_width,
                          int& crop_height, int& crop_width,
                          float min_scale=0.08, float max_scale=1.0, float min_as=0.75, float max_as=1.33){
  float total_area = img_height * img_width;
  float area_ratio = 0;
  float target_area = 0;
  float aspect_ratio = 0;
  float flip_coin = 0;

  int attempt = 0;

  while (attempt < 10) {
    // sample scale and area
    caffe_rng_uniform(1, min_scale, max_scale, &area_ratio);
    target_area = total_area * area_ratio;

    caffe_rng_uniform(1, float(0), float(1), &flip_coin);
    if (flip_coin > 0.5){
        std::swap(crop_height, crop_width);
    }

    // sample aspect ratio
    caffe_rng_uniform(1, min_as, max_as, &aspect_ratio);
    crop_height = int(sqrt(target_area / aspect_ratio));
    crop_width = int(sqrt(target_area * aspect_ratio));

    if (crop_height <= img_height && crop_width <= img_width){
      return;
    }
    attempt ++;
  }

  // fallback to normal 256-224 style size crop
  crop_height = img_height / 8 * 7;
  crop_width = img_width / 8 * 7;
}


template<typename Dtype>
void DataTransformer<Dtype>::Transform(const Datum& datum,
                                       Dtype* transformed_data) {


  const string& data = datum.data();
  const int datum_channels = datum.channels();
  const int datum_height = datum.height();
  const int datum_width = datum.width();

  const int crop_size = param_.crop_size();
  const Dtype scale = param_.scale();
  const bool do_mirror = param_.mirror() && Rand(2);
  const bool has_mean_file = param_.has_mean_file();
  const bool has_uint8 = data.size() > 0;
  const bool has_mean_values = mean_values_.size() > 0;
  const bool do_multi_scale = param_.multi_scale();
  vector<pair<int, int> > offset_pairs;
  vector<pair<int, int> > crop_size_pairs;
  cv::Mat multi_scale_bufferM;

  CHECK_GT(datum_channels, 0);
  CHECK_GE(datum_height, crop_size);
  CHECK_GE(datum_width, crop_size);

  Dtype* mean = NULL;
  if (has_mean_file) {
    CHECK_EQ(datum_channels, data_mean_.channels());
    CHECK_EQ(datum_height, data_mean_.height());
    CHECK_EQ(datum_width, data_mean_.width());
    mean = data_mean_.mutable_cpu_data();
  }
  if (has_mean_values) {
    CHECK(mean_values_.size() == 1 ||  datum_channels % mean_values_.size() == 0) <<
     "Specify either 1 mean_value or as many as channels: " << datum_channels;
    if (datum_channels > 1 && mean_values_.size() == 1) {
      // Replicate the mean_value for simplicity
      for (int c = 1; c < datum_channels; ++c) {
        mean_values_.push_back(mean_values_[0]);
      }
    }

    if (mean_values_.size() < datum_channels){
      // Replicate the mean_value group to fill up the datum channels
      size_t group_size = mean_values_.size();
      for (size_t c = group_size; c < datum_channels; ++c){
        mean_values_.push_back(mean_values_[c % group_size]);
      }
    }
  }

  if (!crop_size && do_multi_scale){
    LOG(ERROR)<< "Multi scale augmentation is only activated with crop_size set.";
  }

  int height = datum_height;
  int width = datum_width;

  int h_off = 0;
  int w_off = 0;
  int crop_height = 0;
  int crop_width = 0;
  bool need_imgproc = false;
  if (crop_size) {
    height = crop_size;
    width = crop_size;
    // We only do random crop when we do training.
    if (phase_ == TRAIN) {
      // If in training and we need multi-scale cropping, reset the crop size params
      if (do_multi_scale){
        fillCropSize(datum_height, datum_width, crop_size, crop_size, crop_size_pairs,
                     max_distort_, custom_scale_ratios_);
        int sel = Rand(crop_size_pairs.size());
        crop_height = crop_size_pairs[sel].first;
        crop_width = crop_size_pairs[sel].second;
      }else{
        crop_height = crop_size;
        crop_width = crop_size;
      }
      if (param_.fix_crop()){
        fillFixOffset(datum_height, datum_width, crop_height, crop_width,
                      param_.more_fix_crop(), offset_pairs);
        int sel = Rand(offset_pairs.size());
        h_off = offset_pairs[sel].first;
        w_off = offset_pairs[sel].second;
      }else{
        h_off = Rand(datum_height - crop_height + 1);
        w_off = Rand(datum_width - crop_width + 1);
      }

    } else {
      crop_height = crop_size;
      crop_width = crop_size;
      h_off = (datum_height - crop_size) / 2;
      w_off = (datum_width - crop_size) / 2;
    }
  }

  need_imgproc = do_multi_scale && crop_size && ((crop_height != crop_size) || (crop_width != crop_size));

  Dtype datum_element;
  int top_index, data_index;
  for (int c = 0; c < datum_channels; ++c) {

    // image resize etc needed
    if (need_imgproc){
      cv::Mat M(datum_height, datum_width, has_uint8?CV_8UC1:CV_32FC1);

      //put the datum content to a cvMat
      for (int h = 0; h < datum_height; ++h) {
        for (int w = 0; w < datum_width; ++w) {
          int data_index = (c * datum_height + h) * datum_width + w;
          if (has_uint8) {
            M.at<uchar>(h, w) = static_cast<uint8_t>(data[data_index]);
          }else{
            M.at<float>(h, w) = datum.float_data(data_index);
          }
        }
      }

      //resize the cropped patch to network input size
      cv::Mat cropM(M, cv::Rect(w_off, h_off, crop_width, crop_height));
      cv::resize(cropM, multi_scale_bufferM, cv::Size(crop_size, crop_size));
      cropM.release();
    }
    for (int h = 0; h < height; ++h) {
      for (int w = 0; w < width; ++w) {
        data_index = (c * datum_height + h_off + h) * datum_width + w_off + w;
        if (do_mirror) {
          top_index = (c * height + h) * width + (width - 1 - w);
        } else {
          top_index = (c * height + h) * width + w;
        }
        if (need_imgproc){
          if (has_uint8){
        	  if (param_.is_flow() && do_mirror && c%2 == 0)
        		  datum_element = 255 - static_cast<Dtype>(multi_scale_bufferM.at<uint8_t>(h, w));
        	  else
        		  datum_element = static_cast<Dtype>(multi_scale_bufferM.at<uint8_t>(h, w));
          }else {
        	  if (param_.is_flow() && do_mirror && c%2 == 0)
        		  datum_element = 255 - static_cast<Dtype>(multi_scale_bufferM.at<float>(h, w));
        	  else
        		  datum_element = static_cast<Dtype>(multi_scale_bufferM.at<float>(h, w));
          }
        }else {
          if (has_uint8) {
        	  if (param_.is_flow() && do_mirror && c%2 == 0)
        		  datum_element = 255 - static_cast<Dtype>(static_cast<uint8_t>(data[data_index]));
        	  else
        		  datum_element = static_cast<Dtype>(static_cast<uint8_t>(data[data_index]));
          } else {
        	  if (param_.is_flow() && do_mirror && c%2 == 0)
        		  datum_element = 255 - datum.float_data(data_index);
        	  else
        		  datum_element = datum.float_data(data_index);
          }
        }
        if (has_mean_file) {
          if (do_multi_scale) {
            int fixed_data_index = (c * datum_height +  h) * datum_width + w;
            transformed_data[top_index] =
                (datum_element - mean[fixed_data_index]) * scale;
          }else{
            transformed_data[top_index] =
                (datum_element - mean[data_index]) * scale;
          }
        } else {
          if (has_mean_values) {
            transformed_data[top_index] =
                (datum_element - mean_values_[c]) * scale;
          } else {
            transformed_data[top_index] = datum_element * scale;
          }
        }
      }
    }
  }
  multi_scale_bufferM.release();
}

template<typename Dtype>
void DataTransformer<Dtype>::Transform(const Datum& datum_data, const Datum& datum_label, 
                                       Blob<Dtype>* transformed_data, Blob<Dtype>* transformed_label, int batch_iter) {

  CHECK_EQ(datum_data.height(), datum_label.height());
  CHECK_EQ(datum_data.width(), datum_label.width());

  const string& data = datum_data.data();
  const string& label = datum_label.data();
  const int datum_channels = datum_data.channels();
  const int datum_height = datum_data.height();
  const int datum_width = datum_data.width();

  float lower_scale = 1, upper_scale = 1;
  if (param_.scale_ratios_size() == 2)
  {
    lower_scale = param_.scale_ratios(0);
    upper_scale = param_.scale_ratios(1);
  }

  const Dtype scale = param_.scale();
  const bool do_mirror = param_.mirror() && Rand(2);
  const bool has_mean_values = mean_values_.size() > 0;
  if (param_.has_mean_file()) {
    NOT_IMPLEMENTED;
  }
  const int stride = param_.stride();

  if (has_mean_values) {
    CHECK(mean_values_.size() == 1 || mean_values_.size() == datum_channels || datum_channels % mean_values_.size() == 0) <<
     "Specify either 1 mean_value or as many as channels: " << datum_channels;
    if (datum_channels > 1 && datum_channels % mean_values_.size() == 0) {
      // Replicate the mean_value for simplicity
      int temp = mean_values_.size();
      for (int c = mean_values_.size(); c < datum_channels; ++c) {
        mean_values_.push_back(mean_values_[c - temp]);
      }
    }
  }

  float scale_ratios = Rand(lower_scale, upper_scale);
  int height = int(datum_height * scale_ratios + 0.5);
  int width = int(datum_width * scale_ratios + 0.5);
  int crop_height = ((height - 1) / stride + 1) * stride;
  int crop_width = ((width - 1) / stride + 1) * stride;
  
  if (param_.has_crop_size())
  {
    crop_height = param_.crop_size();
    crop_width = param_.crop_size();
  }
  else if (param_.has_crop_height() && param_.has_crop_width())
  {
    crop_height = param_.crop_height();
    crop_width = param_.crop_width();
  }

  int h_off, w_off;
  if (height < crop_height)
    h_off = -Rand(crop_height - height + 1);
  else
    h_off = Rand(height - crop_height + 1);

  if (width < crop_width)
    w_off = -Rand(crop_width - width + 1);
  else
    w_off = Rand(width - crop_width + 1);

  if (batch_iter == 0)
  {
    transformed_data->Reshape(transformed_data->num(), datum_channels, crop_height, crop_width);
    transformed_label->Reshape(transformed_data->num(), datum_label.channels(), crop_height, crop_width);
  }

  float angl = 0; 
  if (param_.rand_rotate_size() > 0 && Rand(2)){
    CHECK_EQ(this->param_.rand_rotate_size(), 2) << "Exactly two rand_rotate param required";
    angl = Rand(param_.rand_rotate(0), param_.rand_rotate(1));
  }

  float rand_sigma = 0;
  if (param_.gaussian_blur() && Rand(2)) {
    rand_sigma = Rand(0.0, 0.6);
  }

  //for image data
  
  int top_index;
  Dtype* ptr = transformed_data->mutable_cpu_data() + transformed_data->offset(batch_iter);
  for (int c = 0; c < datum_channels; ++c) {
    cv::Mat M(datum_height, datum_width, CV_8UC1);
    for (int h = 0; h < datum_height; ++h)
      for (int w = 0; w < datum_width; ++w) 
      {
        int data_index = (c * datum_height + h) * datum_width + w;
        M.at<uchar>(h, w) = static_cast<uint8_t>(data[data_index]);
      }

    cv::resize(M, M, cv::Size(width, height));

    if (angl != 0)
       Rotation(M, angl, false, uint8_t(mean_values_[c]));
    if (rand_sigma != 0)
       cv::GaussianBlur(M, M, cv::Size( 5, 5 ), rand_sigma, rand_sigma);
    
    for (int h = 0; h < crop_height; ++h)
      for (int w = 0; w < crop_width; ++w)
      {
        if (do_mirror) 
          top_index = (c * crop_height + h) * crop_width + (crop_width - 1 - w);
        else 
          top_index = (c * crop_height + h) * crop_width + w;

        if (has_mean_values) 
          ptr[top_index] =((h+h_off)>=0) && ((h+h_off)<height) && ((w+w_off)>=0) && ((w+w_off)<width) ? (static_cast<Dtype>(M.at<uint8_t>(h+h_off, w+w_off)) - mean_values_[c]) * scale : 0;
        else 
          ptr[top_index] = ((h+h_off)>=0) && ((h+h_off)<height) && ((w+w_off)>=0) && ((w+w_off)<width) ? static_cast<Dtype>(M.at<uint8_t>(h+h_off, w+w_off)) * scale : 0;
      }
    M.release();
  }

  //for label

  ptr = transformed_label->mutable_cpu_data() + transformed_label->offset(batch_iter);
  for (int c = 0; c < datum_label.channels(); ++c) {
    cv::Mat M(datum_height, datum_width, CV_8UC1);
    for (int h = 0; h < datum_height; ++h)
      for (int w = 0; w < datum_width; ++w)
      {
        int data_index = (c * datum_height + h) * datum_width + w;
        M.at<uchar>(h, w) = static_cast<uint8_t>(label[data_index]);
      }

    cv::resize(M, M, cv::Size(width, height), 0, 0, CV_INTER_NN);

    if (angl != 0)
       Rotation(M, angl, true, param_.ignore_label());

    for (int h = 0; h < crop_height; ++h)
      for (int w = 0; w < crop_width; ++w) 
      {
        if (do_mirror) 
          top_index = (c * crop_height + h) * crop_width + (crop_width - 1 - w);
        else 
          top_index = (c * crop_height + h) * crop_width + w;

        ptr[top_index] = ((h+h_off)>=0) && ((h+h_off)<height) && ((w+w_off)>=0) && ((w+w_off)<width) ? static_cast<Dtype>(M.at<uint8_t>(h+h_off, w+w_off)) : param_.ignore_label();
      }
    M.release();
  }
}

template<typename Dtype>
void DataTransformer<Dtype>::Transform_aug(Datum& datum_label, bool aug_data) {

  string* label = datum_label.mutable_data();

  const int datum_height = datum_label.height();
  const int datum_width = datum_label.width();

  

  if (!aug_data)
  {
    int max_idx = 0;
    for (int c = 1; c <=1; ++c)
      for (int h = 0; h < datum_height; ++h)
        for (int w = 0; w < datum_width; ++w)
        {
          int data_index = (c * datum_height + h) * datum_width + w;
          if ((int)(*label)[data_index] != param_.ignore_label())
            max_idx = std::max(max_idx, (int)(*label)[data_index]);
        }
    if (max_idx == 0)
      return;
    int choose_idx = Rand(max_idx) + 1;

    for (int c = 0; c <datum_label.channels(); ++c)
      for (int h = 0; h < datum_height; ++h)
        for (int w = 0; w < datum_width; ++w)
        {
          int data_index = (c * datum_height + h) * datum_width + w;
          (*label)[data_index] = ((uint8_t)(*label)[data_index] == choose_idx) ? 1 : 0;
        }
  }
  else
  {
    int max_idx = 0;
    cv::Mat orig_inst(datum_height, datum_width, CV_8UC1);

    for (int c = 2; c < datum_label.channels(); ++c)
      for (int h = 0; h < datum_height; ++h)
        for (int w = 0; w < datum_width; ++w)
        {
          int data_index = (c * datum_height + h) * datum_width + w;
          orig_inst.at<uchar>(h, w) = (uint8_t)(*label)[data_index];
          if ((int)orig_inst.at<uchar>(h, w) != param_.ignore_label())
            max_idx = std::max(max_idx, (int)orig_inst.at<uchar>(h, w));
        }
    if (max_idx == 0)
    {
      orig_inst.release();
      return;
    }
    cv::Mat inst = cv::Mat::zeros(orig_inst.size(), orig_inst.type());

    int choose_idx = Rand(max_idx) + 1;
    for (int c = 1; c <= 1; ++c)
      for (int h = 0; h < datum_height; ++h)
        for (int w = 0; w < datum_width; ++w)
        {
          int data_index = (c * datum_height + h) * datum_width + w;
          (*label)[data_index] = ((uint8_t)(*label)[data_index] == choose_idx) ? 1 : 0;
        }

    for (int h = 0; h < datum_height; ++h)
        for (int w = 0; w < datum_width; ++w)
          inst.at<uchar>(h, w) = (orig_inst.at<uchar>(h, w) == choose_idx) ? 1 : 0;

    //rotation -5~5
    double angl = -10 + Rand(int((10 - (-10)) * 1000.0) + 1) / 1000.0;
    Rotation(inst, angl, false, 0);

    //thin-plate splines

    //resize 95%~105%
    cv::resize(inst, inst, cv::Size(datum_width / 8, datum_height / 8), 0, 0);
    cv::resize(inst, inst, cv::Size(datum_width, datum_height), 0, 0);

    float scale_ratios = std::max(Rand(int((1.10 - 0.90) * 1000.0) + 1) / 1000.0, 0.0) + 0.90;

    cv::resize(inst, inst, cv::Size(int(datum_width * scale_ratios), int(datum_height * scale_ratios)), 0, 0);


    double tot_h = 0, tot_w = 0, tot = 0, tot_h2 = 0, tot_w2 = 0, tot2 = 0;
    for (int h = 0; h < datum_height; ++h)
      for (int w = 0; w < datum_width; ++w)
        if (orig_inst.at<uchar>(h, w) == choose_idx)
        {
          tot_h = tot_h + h;
          tot_w = tot_w + w;
          tot++;
        }

    int min_h = inst.rows, max_h = 0, min_w = inst.cols, max_w = 0;
    double th = Rand(1000) / 1000.0;
    for (int h = 0; h < inst.rows; ++h)
      for (int w = 0; w < inst.cols; ++w)
        if (inst.at<uchar>(h, w) > th)
        {
          min_h = std::min(min_h, h);
          max_h = std::max(max_h, h);
          min_w = std::min(min_w, w);
          max_w = std::max(max_w, w);

          tot_h2 = tot_h2 + h;
          tot_w2 = tot_w2 + w;
          tot2++;
        }

    if (tot2>=1)
    {
      int shift_h = (Rand(1000) / 1000.0 * 0.2 - 0.1) * (max_h - min_h);
      int shift_w = (Rand(1000) / 1000.0 * 0.2 - 0.1) * (max_w - min_w);

      shift_h += int(tot_h / tot - tot_h2 / tot2);
      shift_w += int(tot_w / tot - tot_w2 / tot2);

      

      orig_inst = cv::Mat::zeros(orig_inst.size(), orig_inst.type());
      for (int h = 0; h < inst.rows; ++h)
        for (int w = 0; w < inst.cols; ++w)
          if (inst.at<uchar>(h, w) > th &&  h+shift_h>=0 && h+shift_h<orig_inst.rows && w+shift_w>=0 && w+shift_w<orig_inst.cols)
            orig_inst.at<uchar>(h+shift_h, w+shift_w) = 1;
    }
    for (int c = 2; c < datum_label.channels(); ++c)
      for (int h = 0; h < datum_height; ++h)
        for (int w = 0; w < datum_width; ++w)
        {
          int data_index = (c * datum_height + h) * datum_width + w;
          (*label)[data_index] = static_cast<char>(orig_inst.at<uchar>(h, w));
        }
    inst.release();
    orig_inst.release();
  }
}


template<typename Dtype>
void DataTransformer<Dtype>::Transform(const Datum& datum,
                                       Blob<Dtype>* transformed_blob) {
  // If datum is encoded, decoded and transform the cv::image.
  if (datum.encoded()) {
    CHECK(!(param_.force_color() && param_.force_gray()))
        << "cannot set both force_color and force_gray";
    cv::Mat cv_img;
    if (param_.force_color() || param_.force_gray()) {
    // If force_color then decode in color otherwise decode in gray.
      cv_img = DecodeDatumToCVMat(datum, param_.force_color());
    } else {
      cv_img = DecodeDatumToCVMatNative(datum);
    }
    // Transform the cv::image into blob.
    return Transform(cv_img, transformed_blob);
  } else {
    if (param_.force_color() || param_.force_gray()) {
      LOG(ERROR) << "force_color and force_gray only for encoded datum";
    }
  }

  const int crop_size = param_.crop_size();
  const int datum_channels = datum.channels();
  const int datum_height = datum.height();
  const int datum_width = datum.width();

  // Check dimensions.
  const int channels = transformed_blob->channels();
  const int height = transformed_blob->height();
  const int width = transformed_blob->width();
  const int num = transformed_blob->num();

  CHECK_EQ(channels, datum_channels);
  CHECK_LE(height, datum_height);
  CHECK_LE(width, datum_width);
  CHECK_GE(num, 1);

  if (crop_size) {
    CHECK_EQ(crop_size, height);
    CHECK_EQ(crop_size, width);
  } else {
    CHECK_EQ(datum_height, height);
    CHECK_EQ(datum_width, width);
  }

  Dtype* transformed_data = transformed_blob->mutable_cpu_data();
  Transform(datum, transformed_data);
}

template<typename Dtype>
void DataTransformer<Dtype>::Transform(const vector<Datum> & datum_vector,
                                       Blob<Dtype>* transformed_blob) {
  const int datum_num = datum_vector.size();
  const int num = transformed_blob->num();
  const int channels = transformed_blob->channels();
  const int height = transformed_blob->height();
  const int width = transformed_blob->width();

  CHECK_GT(datum_num, 0) << "There is no datum to add";
  CHECK_LE(datum_num, num) <<
    "The size of datum_vector must be no greater than transformed_blob->num()";
  Blob<Dtype> uni_blob(1, channels, height, width);
  for (int item_id = 0; item_id < datum_num; ++item_id) {
    int offset = transformed_blob->offset(item_id);
    uni_blob.set_cpu_data(transformed_blob->mutable_cpu_data() + offset);
    Transform(datum_vector[item_id], &uni_blob);
  }
}

template<typename Dtype>
void DataTransformer<Dtype>::Transform(const vector<cv::Mat> & mat_vector,
                                       Blob<Dtype>* transformed_blob) {
  const int mat_num = mat_vector.size();
  const int num = transformed_blob->num();
  const int channels = transformed_blob->channels();
  const int height = transformed_blob->height();
  const int width = transformed_blob->width();

  CHECK_GT(mat_num, 0) << "There is no MAT to add";
  CHECK_EQ(mat_num, num) <<
    "The size of mat_vector must be equals to transformed_blob->num()";
  Blob<Dtype> uni_blob(1, channels, height, width);
  for (int item_id = 0; item_id < mat_num; ++item_id) {
    int offset = transformed_blob->offset(item_id);
    uni_blob.set_cpu_data(transformed_blob->mutable_cpu_data() + offset);
    Transform(mat_vector[item_id], &uni_blob);
  }
}

template<typename Dtype>
void DataTransformer<Dtype>::Transform(const cv::Mat& cv_img,
                                       Blob<Dtype>* transformed_blob) {
  const int crop_size = param_.crop_size();
  const int img_channels = cv_img.channels();
  const int img_height = cv_img.rows;
  const int img_width = cv_img.cols;

  // Check dimensions.
  const int channels = transformed_blob->channels();
  const int height = transformed_blob->height();
  const int width = transformed_blob->width();
  const int num = transformed_blob->num();

  CHECK_EQ(channels, img_channels);

  if (!org_size_proc_) {
    CHECK_LE(height, img_height);
    CHECK_LE(width, img_width);
  }
  CHECK_GE(num, 1);

  CHECK(cv_img.depth() == CV_8U) << "Image data type must be unsigned byte";

  const Dtype scale = param_.scale();
  const bool do_mirror = param_.mirror() && Rand(2);
  const bool has_mean_file = param_.has_mean_file();
  const bool has_mean_values = mean_values_.size() > 0;
  const bool do_multi_scale = param_.multi_scale();

  vector<pair<int, int> > offset_pairs;
  vector<pair<int, int> > crop_size_pairs;

  cv::Mat cv_cropped_img;

  CHECK_GT(img_channels, 0);
  if (!org_size_proc_) {
    CHECK_GE(img_height, crop_size);
    CHECK_GE(img_width, crop_size);
  }

  Dtype *mean = NULL;
  if (has_mean_file) {
    CHECK_EQ(img_channels, data_mean_.channels());
    CHECK_EQ(img_height, data_mean_.height());
    CHECK_EQ(img_width, data_mean_.width());
    mean = data_mean_.mutable_cpu_data();
  }
  if (has_mean_values) {
    CHECK(mean_values_.size() == 1 || mean_values_.size() == img_channels) <<
                                                                           "Specify either 1 mean_value or as many as channels: " <<
                                                                           img_channels;
    if (img_channels > 1 && mean_values_.size() == 1) {
      // Replicate the mean_value for simplicity
      for (int c = 1; c < img_channels; ++c) {
        mean_values_.push_back(mean_values_[0]);
      }
    }
  }

  int h_off = 0;
  int w_off = 0;
  int crop_height = 0;
  int crop_width = 0;

  if (!org_size_proc_) {
    if (crop_size) {
      CHECK_EQ(crop_size, height);
      CHECK_EQ(crop_size, width);
      // We only do random crop when we do training.
      if (phase_ == TRAIN) {
        if (do_multi_scale) {
          fillCropSize(img_height, img_width, crop_size, crop_size, crop_size_pairs,
                       max_distort_, custom_scale_ratios_);
          int sel = Rand(crop_size_pairs.size());
          crop_height = crop_size_pairs[sel].first;
          crop_width = crop_size_pairs[sel].second;
        } else {
          crop_height = crop_size;
          crop_width = crop_size;
        }
        if (param_.fix_crop()) {
          fillFixOffset(img_height, img_width, crop_height, crop_width,
                        param_.more_fix_crop(), offset_pairs);
          int sel = Rand(offset_pairs.size());
          h_off = offset_pairs[sel].first;
          w_off = offset_pairs[sel].second;
        } else {
          h_off = Rand(img_height - crop_height + 1);
          w_off = Rand(img_width - crop_width + 1);
        }
      } else {
        h_off = (img_height - crop_size) / 2;
        w_off = (img_width - crop_size) / 2;
        crop_width = crop_size;
        crop_height = crop_size;
      }
      cv::Rect roi(w_off, h_off, crop_width, crop_height);
      // if resize needed, first put the resized image into a buffer, then copy back.
      if (do_multi_scale && ((crop_height != crop_size) || (crop_width != crop_size))) {
        cv::Mat crop_bufferM(cv_img, roi);
        cv::resize(crop_bufferM, cv_cropped_img, cv::Size(crop_size, crop_size));
        crop_bufferM.release();
      } else {
        cv_cropped_img = cv_img(roi);
      }
    } else {
      CHECK_EQ(img_height, height);
      CHECK_EQ(img_width, width);
      cv_cropped_img = cv_img;
    }
  }else{
    CHECK(crop_size>0)<<"in original image processing mode, crop size must be specified";
    CHECK_EQ(crop_size, height);
    CHECK_EQ(crop_size, width);
    if (phase_ == TRAIN) {
      // in training, we randomly crop different sized crops
      sampleRandomCropSize(img_height, img_width, crop_height, crop_width, 0.6, 1.0, 0.8, 1.25);



      h_off = (crop_height < img_height)?Rand(img_height - crop_height):0;
      w_off = (crop_width < img_width)?Rand(img_width - crop_width):0;
    }else{
      // in testing, we first resize the image to sizeof (8/7*crop_size) then crop the central patch
      h_off = img_height / 14;
      w_off = img_width / 14;
      crop_height = img_height / 8 * 7;
      crop_width = img_width / 8 * 7;
    }

    cv::Rect roi(w_off, h_off, crop_width, crop_height);

    // resize is always needed in original image mode
    cv::Mat crop_bufferM(cv_img, roi);
    cv::resize(crop_bufferM, cv_cropped_img, cv::Size(crop_size, crop_size), 0, 0, CV_INTER_CUBIC);
    crop_bufferM.release();
  }

  CHECK(cv_cropped_img.data);

  Dtype *transformed_data = transformed_blob->mutable_cpu_data();
  int top_index;
  for (int h = 0; h < height; ++h) {
    const uchar *ptr = cv_cropped_img.ptr<uchar>(h);
    int img_index = 0;
    for (int w = 0; w < width; ++w) {
      for (int c = 0; c < img_channels; ++c) {
        if (do_mirror) {
          top_index = (c * height + h) * width + (width - 1 - w);
        } else {
          top_index = (c * height + h) * width + w;
        }
        // int top_index = (c * height + h) * width + w;
        Dtype pixel = static_cast<Dtype>(ptr[img_index++]);
        if (has_mean_file) {
          //we will use a fixed position of mean map for multi-scale.
          int mean_index = (do_multi_scale) ?
                           (c * img_height + h) * img_width + w
                                            : (c * img_height + h_off + h) * img_width + w_off + w;
          if (param_.is_flow() && do_mirror && c % 2 == 0)
            transformed_data[top_index] =
                (255 - pixel - mean[mean_index]) * scale;
          else
            transformed_data[top_index] =
                (pixel - mean[mean_index]) * scale;
        } else {
          if (has_mean_values) {
            if (param_.is_flow() && do_mirror && c % 2 == 0)
              transformed_data[top_index] =
                  (255 - pixel - mean_values_[c]) * scale;
            else
              transformed_data[top_index] =
                  (pixel - mean_values_[c]) * scale;
          } else {
            if (param_.is_flow() && do_mirror && c % 2 == 0)
              transformed_data[top_index] = (255 - pixel) * scale;
            else
              transformed_data[top_index] = pixel * scale;
          }
        }
      }
    }
  }
  cv_cropped_img.release();
}

template<typename Dtype>
void DataTransformer<Dtype>::Transform(Blob<Dtype>* input_blob,
                                       Blob<Dtype>* transformed_blob) {
  const int crop_size = param_.crop_size();
  const int input_num = input_blob->num();
  const int input_channels = input_blob->channels();
  const int input_height = input_blob->height();
  const int input_width = input_blob->width();

  if (transformed_blob->count() == 0) {
    // Initialize transformed_blob with the right shape.
    if (crop_size) {
      transformed_blob->Reshape(input_num, input_channels,
                                crop_size, crop_size);
    } else {
      transformed_blob->Reshape(input_num, input_channels,
                                input_height, input_width);
    }
  }

  const int num = transformed_blob->num();
  const int channels = transformed_blob->channels();
  const int height = transformed_blob->height();
  const int width = transformed_blob->width();
  const int size = transformed_blob->count();

  CHECK_LE(input_num, num);
  CHECK_EQ(input_channels, channels);
  CHECK_GE(input_height, height);
  CHECK_GE(input_width, width);


  const Dtype scale = param_.scale();
  const bool do_mirror = param_.mirror() && Rand(2);
  const bool has_mean_file = param_.has_mean_file();
  const bool has_mean_values = mean_values_.size() > 0;

  int h_off = 0;
  int w_off = 0;
  if (crop_size) {
    CHECK_EQ(crop_size, height);
    CHECK_EQ(crop_size, width);
    // We only do random crop when we do training.
    if (phase_ == TRAIN) {
      h_off = Rand(input_height - crop_size + 1);
      w_off = Rand(input_width - crop_size + 1);
    } else {
      h_off = (input_height - crop_size) / 2;
      w_off = (input_width - crop_size) / 2;
    }
  } else {
    CHECK_EQ(input_height, height);
    CHECK_EQ(input_width, width);
  }

  Dtype* input_data = input_blob->mutable_cpu_data();
  if (has_mean_file) {
    CHECK_EQ(input_channels, data_mean_.channels());
    CHECK_EQ(input_height, data_mean_.height());
    CHECK_EQ(input_width, data_mean_.width());
    for (int n = 0; n < input_num; ++n) {
      int offset = input_blob->offset(n);
      caffe_sub(data_mean_.count(), input_data + offset,
            data_mean_.cpu_data(), input_data + offset);
    }
  }

  if (has_mean_values) {
    CHECK(mean_values_.size() == 1 || mean_values_.size() == input_channels) <<
     "Specify either 1 mean_value or as many as channels: " << input_channels;
    if (mean_values_.size() == 1) {
      caffe_add_scalar(input_blob->count(), -(mean_values_[0]), input_data);
    } else {
      for (int n = 0; n < input_num; ++n) {
        for (int c = 0; c < input_channels; ++c) {
          int offset = input_blob->offset(n, c);
          caffe_add_scalar(input_height * input_width, -(mean_values_[c]),
            input_data + offset);
        }
      }
    }
  }

  Dtype* transformed_data = transformed_blob->mutable_cpu_data();

  for (int n = 0; n < input_num; ++n) {
    int top_index_n = n * channels;
    int data_index_n = n * channels;
    for (int c = 0; c < channels; ++c) {
      int top_index_c = (top_index_n + c) * height;
      int data_index_c = (data_index_n + c) * input_height + h_off;
      for (int h = 0; h < height; ++h) {
        int top_index_h = (top_index_c + h) * width;
        int data_index_h = (data_index_c + h) * input_width + w_off;
        if (do_mirror) {
          int top_index_w = top_index_h + width - 1;
          for (int w = 0; w < width; ++w) {
        	  if (param_.is_flow() && c%2 == 0)
        		  transformed_data[top_index_w-w] = 255 - input_data[data_index_h + w];
        	  else
        		  transformed_data[top_index_w-w] = input_data[data_index_h + w];
          }
        } else {
          for (int w = 0; w < width; ++w) {
            transformed_data[top_index_h + w] = input_data[data_index_h + w];
          }
        }
      }
    }
  }
  if (scale != Dtype(1)) {
    DLOG(INFO) << "Scale: " << scale;
    caffe_scal(size, scale, transformed_data);
  }
}

template<typename Dtype>
vector<int> DataTransformer<Dtype>::InferBlobShape(const Datum& datum) {
  if (datum.encoded()) {
    CHECK(!(param_.force_color() && param_.force_gray()))
        << "cannot set both force_color and force_gray";
    cv::Mat cv_img;
    if (param_.force_color() || param_.force_gray()) {
    // If force_color then decode in color otherwise decode in gray.
      cv_img = DecodeDatumToCVMat(datum, param_.force_color());
    } else {
      cv_img = DecodeDatumToCVMatNative(datum);
    }
    // InferBlobShape using the cv::image.
    return InferBlobShape(cv_img);
  }

  const int crop_size = param_.crop_size();
  const int datum_channels = datum.channels();
  const int datum_height = datum.height();
  const int datum_width = datum.width();
  // Check dimensions.
  CHECK_GT(datum_channels, 0);
  CHECK_GE(datum_height, crop_size);
  CHECK_GE(datum_width, crop_size);
  // Build BlobShape.
  vector<int> shape(4);
  shape[0] = 1;
  shape[1] = datum_channels;
  shape[2] = (crop_size)? crop_size: datum_height;
  shape[3] = (crop_size)? crop_size: datum_width;
  return shape;
}

template<typename Dtype>
vector<int> DataTransformer<Dtype>::InferBlobShape(
    const vector<Datum> & datum_vector) {
  const int num = datum_vector.size();
  CHECK_GT(num, 0) << "There is no datum to in the vector";
  // Use first datum in the vector to InferBlobShape.
  vector<int> shape = InferBlobShape(datum_vector[0]);
  // Adjust num to the size of the vector.
  shape[0] = num;
  return shape;
}

template<typename Dtype>
vector<int> DataTransformer<Dtype>::InferBlobShape(const cv::Mat& cv_img) {
  const int crop_size = param_.crop_size();
  const int img_channels = cv_img.channels();
  const int img_height = cv_img.rows;
  const int img_width = cv_img.cols;
  // Check dimensions.
  CHECK_GT(img_channels, 0);
  if (!org_size_proc_) {
    CHECK_GE(img_height, crop_size);
    CHECK_GE(img_width, crop_size);
  }
  // Build BlobShape.
  vector<int> shape(4);
  shape[0] = 1;
  shape[1] = img_channels;
  shape[2] = (crop_size)? crop_size: img_height;
  shape[3] = (crop_size)? crop_size: img_width;
  return shape;
}

template<typename Dtype>
vector<int> DataTransformer<Dtype>::InferBlobShape(
    const vector<cv::Mat> & mat_vector) {
  const int num = mat_vector.size();
  CHECK_GT(num, 0) << "There is no cv_img to in the vector";
  // Use first cv_img in the vector to InferBlobShape.
  vector<int> shape = InferBlobShape(mat_vector[0]);
  // Adjust num to the size of the vector.
  shape[0] = num;
  return shape;
}

template <typename Dtype>
void DataTransformer<Dtype>::InitRand() {
  const bool needs_rand = param_.mirror() || (phase_ == TRAIN);
  if (needs_rand) {
    const unsigned int rng_seed =caffe_rng_rand();
    rng_.reset(new Caffe::RNG(rng_seed));
  } else {
    rng_.reset();
  }
}

template <typename Dtype>
int DataTransformer<Dtype>::Rand(int n) {
  CHECK(rng_);
  CHECK_GT(n, 0);
  caffe::rng_t* rng =
      static_cast<caffe::rng_t*>(rng_->generator());
  return ((*rng)() % n);
}


template <typename Dtype>
float DataTransformer<Dtype>::Rand(float l, float r) {
  CHECK(rng_);
  CHECK_GE(r, l); 
  return std::min(std::max(Rand(int((r - l) * 10000.0) + 1) / 10000.0f + l, l), r);
}

template <typename Dtype>
void DataTransformer<Dtype>::Rotation(cv::Mat& src, int degree, bool islabel, uint8_t mean_v){
  int height = src.size().height;
  int width = src.size().width;

  cv::Point2f center = cv::Point2f(width / 2, height / 2);
  cv::Mat map_matrix = cv::getRotationMatrix2D(center, degree, 1.0);
  if (islabel){
    cv::warpAffine(src, src, map_matrix, src.size(), cv::INTER_NEAREST, cv::BORDER_CONSTANT, cv::Scalar(mean_v));
  } else{
    cv::warpAffine(src, src, map_matrix, src.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(mean_v));
  }
}

INSTANTIATE_CLASS(DataTransformer);

}  // namespace caffe
