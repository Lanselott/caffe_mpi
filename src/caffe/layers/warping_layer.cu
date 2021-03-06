#include <algorithm>
#include <vector>
#include <cmath>

#include "caffe/layer.hpp"
#include "caffe/util/math_functions.hpp"
#include "caffe/vision_layers.hpp"

namespace caffe {

template <typename Dtype>
__global__ void warping_forward(const int nthreads, const Dtype* in_data, const Dtype* flow_data, const int channels, const int height, const int width, Dtype* out_data) 
{
  CUDA_KERNEL_LOOP(index, nthreads) {
    const int w = index % width;
    const int h = (index / width) % height;
    const int c = (index / width / height) % channels;
    const int n = index / width / height / channels;
    const Dtype dh = flow_data[((n * 2 + 1) * height + h) * width + w];
    const Dtype dw = flow_data[((n * 2 + 0) * height + h) * width + w];
    out_data[index] = 0;

    int th = std::floor(Dtype(h) + dh), tw = std::floor(Dtype(w)+dw);
    Dtype weight = (1 - (Dtype(h-th) + dh)) * (1 - (Dtype(w-tw) + dw));
    int nth = (th >= 0) ? (th < height ? th : height - 1) : 0;
    int ntw = (tw >= 0) ? (tw < width ? tw : width - 1) : 0;
  	out_data[index] += in_data[((n * channels + c) * height + nth) * width + ntw]  * weight;

  	th = th + 1, weight =  (1 - (Dtype(th-h) - dh)) * (1 - (Dtype(w-tw) + dw));
    nth = (th >= 0) ? (th < height ? th : height - 1) : 0;
  	out_data[index] += in_data[((n * channels + c) * height + nth) * width + ntw]  * weight;

  	th = th - 1, tw = tw + 1, weight =  (1 - (Dtype(h-th) + dh)) * (1 - (Dtype(tw-w) - dw));
    nth = (th >= 0) ? (th < height ? th : height - 1) : 0;
    ntw = (tw >= 0) ? (tw < width ? tw : width - 1) : 0;
  	out_data[index] += in_data[((n * channels + c) * height + nth) * width + ntw]  * weight;

  	th = th + 1, weight = (1 - (Dtype(th-h) - dh)) * (1 - (Dtype(tw-w) - dw));
    nth = (th >= 0) ? (th < height ? th : height - 1) : 0;
  	out_data[index] += in_data[((n * channels + c) * height + nth) * width + ntw]  * weight;
  }
}






template <typename Dtype>
void WarpingLayer<Dtype>::Forward_gpu(const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top) {
  Dtype* top_data = top[0]->mutable_gpu_data();
  const Dtype* bottom_data =  bottom[0]->gpu_data();
  const Dtype* flow_data = bottom[1]->gpu_data();
  int nthreads = top[0]->count();

  warping_forward<Dtype><<<CAFFE_GET_BLOCKS(nthreads), CAFFE_CUDA_NUM_THREADS>>>(
    nthreads, bottom_data, flow_data, channels_, height_, width_, top_data);
}



template <typename Dtype>
__global__ void warping_backward_data(const int nthreads, const Dtype* in_data, const Dtype* head, const Dtype* edge, const int channels, const int height, const int width, Dtype* out_data) 
{
  CUDA_KERNEL_LOOP(index, nthreads) {
    const int w = index % width;
    const int h = (index / width) % height;
    const int c = (index / width / height) % channels;
    const int n = index / width / height / channels;

    out_data[index] = 0;
    for (int i = head[(n * height + h) * width + w]; i!= -1; i = edge[i])
    	out_data[index] += edge[i+3] * in_data[((n *channels + c)*height + static_cast<int>(edge[i+1])) * width + static_cast<int>(edge[i+2])];
  }
}

template <typename Dtype>
__global__ void warping_backward_flow(const int nthreads, const Dtype* in_data, const Dtype* bottom_data, const Dtype* flow_data, const int channels, const int height, const int width, const int spatial_dim, Dtype* out_data) 
{
  CUDA_KERNEL_LOOP(index, nthreads) {
    const int w = index % width;
    const int h = (index / width) % height;
    const int n = index / width / height;

    const int index_w = ((n * 2 + 0) * height + h) * width + w;
    const int index_h = index_w + spatial_dim;

    out_data[index_h] = 0;
    out_data[index_w] = 0;

    const Dtype dh = flow_data[index_h];
    const Dtype dw = flow_data[index_w];

    const Dtype* top_diff = in_data + n * channels * spatial_dim +  h * width + w;

    for (int c=0; c<channels; c++)
    {
    	int th = std::floor(Dtype(h) + dh), tw = std::floor(Dtype(w)+dw);

      int nth = (th >= 0) ? (th < height ? th : height - 1) : 0;
      int ntw = (tw >= 0) ? (tw < width ? tw : width - 1) : 0;

  		out_data[index_h] -= (1 - (Dtype(w-tw) + dw)) * bottom_data[((n * channels + c) * height + nth) * width + ntw] * (*top_diff);
  		out_data[index_w] -= (1 - (Dtype(h-th) + dh)) * bottom_data[((n * channels + c) * height + nth) * width + ntw] * (*top_diff);


  		th = th + 1;
      nth = (th >= 0) ? (th < height ? th : height - 1) : 0;

  		out_data[index_h] += (1 - (Dtype(w-tw) + dw)) * bottom_data[((n * channels + c) * height + nth) * width + ntw] * (*top_diff);
  		out_data[index_w] -= (1 - (Dtype(th-h) - dh)) * bottom_data[((n * channels + c) * height + nth) * width + ntw] * (*top_diff);


  		th = th - 1, tw = tw + 1;
      nth = (th >= 0) ? (th < height ? th : height - 1) : 0;
      ntw = (tw >= 0) ? (tw < width ? tw : width - 1) : 0;

  		out_data[index_h] -= (1 - (Dtype(tw-w) - dw)) * bottom_data[((n * channels + c) * height + nth) * width + ntw] * (*top_diff);
  		out_data[index_w] += (1 - (Dtype(h-th) + dh)) * bottom_data[((n * channels + c) * height + nth) * width + ntw] * (*top_diff);
    	

  		th = th + 1;
      nth = (th >= 0) ? (th < height ? th : height - 1) : 0;

  		out_data[index_h] += (1 - (Dtype(tw-w) - dw)) * bottom_data[((n * channels + c) * height + nth) * width + ntw] * (*top_diff);
  		out_data[index_w] += (1 - (Dtype(th-h) - dh)) * bottom_data[((n * channels + c) * height + nth) * width + ntw] * (*top_diff);

    	top_diff += spatial_dim;
    }
  }
}

template <typename Dtype>
void  WarpingLayer<Dtype>::Backward_gpu(const vector<Blob<Dtype>*>& top,
      const vector<bool>& propagate_down, const vector<Blob<Dtype>*>& bottom) {
  
  const Dtype* top_diff = top[0]->gpu_diff();
  const Dtype* flow_data = bottom[1]->gpu_data();
  const Dtype* flow_data_cpu = bottom[1]->cpu_data();

  Dtype* edge = edge_.mutable_cpu_data();
  edge_cnt_ = 0;
  Dtype* head = head_.mutable_cpu_data();
  caffe_set(head_.count(), Dtype(-1), head);

  //build the edge map
  for (int n=0; n<bottom[1]->num(); n++)
  	for (int h=0; h<bottom[1]->height(); h++)
  		for (int w=0; w<bottom[1]->width(); w++)
  		{
  			Dtype dh = flow_data_cpu[((n * 2 + 1) * height_ + h) * width_ + w];
  			Dtype dw = flow_data_cpu[((n * 2 + 0) * height_ + h) * width_ + w];
  			for (int i=0; i<2; i++)
  				for (int j=0; j<2; j++)
  				{
  					int th = std::floor(Dtype(h) + dh) + i, tw = std::floor(Dtype(w) + dw) + j;
  					Dtype weight = (1 - abs(Dtype(h + dh - th))) * (1 - abs(Dtype(w+ dw - tw)));

            th = (th >= 0) ? (th < height_ ? th : height_ - 1) : 0;
            tw = (tw >= 0) ? (tw < width_ ? tw : width_ - 1) : 0;
						int offset = (n * height_ + th) * width_ + tw;
						edge[edge_cnt_] = head[offset];
						head[offset] = static_cast<Dtype>(edge_cnt_);
						edge[edge_cnt_ + 1] = static_cast<Dtype>(h);
						edge[edge_cnt_ + 2] = static_cast<Dtype>(w);
						edge[edge_cnt_ + 3] = weight;
						edge_cnt_ += 4;
  				}
  		}

  if (propagate_down[0])
  {
	  int nthreads = bottom[0]->count();
	  warping_backward_data<Dtype><<<CAFFE_GET_BLOCKS(nthreads), CAFFE_CUDA_NUM_THREADS>>>(
	    nthreads, top_diff, head_.gpu_data(), edge_.gpu_data(), channels_, height_, width_, bottom[0]->mutable_gpu_diff());
  }
  if (propagate_down[1])
  {
	  int nthreads = bottom[1]->num() * spatial_dim_;
	  warping_backward_flow<Dtype><<<CAFFE_GET_BLOCKS(nthreads), CAFFE_CUDA_NUM_THREADS>>>(
	    nthreads, top_diff, bottom[0]->gpu_data(), bottom[1]->gpu_data(), channels_, height_, width_, spatial_dim_, bottom[1]->mutable_gpu_diff());	
  }
}


INSTANTIATE_LAYER_GPU_FUNCS(WarpingLayer);

}  // namespace caffe
