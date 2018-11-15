#include <algorithm>
#include <vector>
 
#include "caffe/layers/rpn_layer.hpp"
#include "caffe/util/math_functions.hpp"
#include <opencv2/opencv.hpp>
/*
	   feat_stride : 4
	   feat_stride : 8
	   feat_stride : 16
	   feat_stride : 32
	   feat_stride : 64
       basesize : 16
       scale : 8
       scale : 16
       scale : 32
       ratio : 0.5
       ratio : 1
       ratio : 2
       boxminsize :16
       per_nms_topn : 0;
       post_nms_topn : 0;
       nms_thresh : 0.3
*/

int debug = 0;
int  tmp[9][4] = {
	{ -83, -39, 100, 56 },
	{ -175, -87, 192, 104 },
	{ -359, -183, 376, 200 },
	{ -55, -55, 72, 72 },
	{ -119, -119, 136, 136 },
	{ -247, -247, 264, 264 },
	{ -35, -79, 52, 96 },
	{ -79, -167, 96, 184 },
	{ -167, -343, 184, 360 }
};

namespace caffe {
 
	template <typename Dtype>
	void RPNLayer<Dtype>::LayerSetUp(
		const vector<Blob<Dtype>*>& bottom, const vector<Blob<Dtype>*>& top){
		feat_strides_.clear();
		anchor_scales_.clear();
		ratios_.clear();
		//feat_stride_ = this->layer_param_.rpn_param().feat_stride(); //frcnn
		//base_size_ = this->layer_param_.rpn_param().basesize();
		min_size_ = this->layer_param_.rpn_param().boxminsize();
		pre_nms_topN_ = this->layer_param_.rpn_param().per_nms_topn();
		post_nms_topN_ = this->layer_param_.rpn_param().post_nms_topn();
		nms_thresh_ = this->layer_param_.rpn_param().nms_thresh();

		//starting get feat_stride,ratio,scale
		//fpn add
		int feat_strides_num = this->layer_param_.rpn_param().feat_stride_size();
		for(int i = 0; i < feat_strides_num; i++){
			feat_strides_.push_back(this->layer_param_.rpn_param().feat_stride(i));
		}

		int scales_num = this->layer_param_.rpn_param().scale_size();
		for(int i = 0; i < scales_num; i++){
			anchor_scales_.push_back(this->layer_param_.rpn_param().scale(i));
		}

		int ratios_num = this->layer_param_.rpn_param().ratio_size();
		for(int i = 0; i < ratios_num; i++){
			ratios_.push_back(this->layer_param_.rpn_param().ratio(i));
		}
	}

	template <typename Dtype>
	void RPNLayer<Dtype>::generate_anchors(int base_size){
		//generate base anchor
		vector<float> base_anchor;
		base_anchor.push_back(0);
		base_anchor.push_back(0);
		base_anchor.push_back(base_size - 1);
		base_anchor.push_back(base_size - 1);
		//enum ratio anchors

		vector<vector<float> >ratio_anchors = ratio_enum(base_anchor);

		for (int i = 0; i < ratio_anchors.size(); ++i)
		{
			vector<vector<float> > tmp = scale_enum(ratio_anchors[i]);
			gen_anchors_.insert(gen_anchors_.end(), tmp.begin(), tmp.end());
		}
	}

	template <typename Dtype>
	vector<vector<float> > RPNLayer<Dtype>::scale_enum(vector<float> anchor){
		vector<vector<float> > result;
		vector<float> reform_anchor = whctrs(anchor);
		float x_ctr = reform_anchor[2];
		float y_ctr = reform_anchor[3];
		float w = reform_anchor[0];
		float h = reform_anchor[1];
		for (int i = 0; i < anchor_scales_.size(); ++i)
		{
			float ws = w * anchor_scales_[i];
			float hs = h *  anchor_scales_[i];
			vector<float> tmp = mkanchor(ws, hs, x_ctr, y_ctr);
			result.push_back(tmp);
		}
		return result;
	}

	template <typename Dtype>
	vector<vector<float> > RPNLayer<Dtype>::ratio_enum(vector<float> anchor){
		//Enumerate a set of anchors
		vector<vector<float> > result;
		vector<float> reform_anchor = whctrs(anchor);
		float x_ctr = reform_anchor[2];
		float y_ctr = reform_anchor[3];
		float size = reform_anchor[0]*reform_anchor[1];
		for(int i = 0; i <ratios_.size(); ++i){
			float size_ratios = size / ratios_[i];
			float ws = round(sqrt(size_ratios));
			float hs = round(ws*ratios_[i]);
			vector<float> tmp = mkanchor(ws, hs, x_ctr, y_ctr);
			result.push_back(tmp);
		}
		return result;
	}

	template <typename Dtype>
	vector<float> RPNLayer<Dtype>::whctrs(vector<float> anchor){
		//return w,h,x center,y center for an anchor
		vector<float> result;
		result.push_back(anchor[2] - anchor[0] + 1); //w
		result.push_back(anchor[3] - anchor[1] + 1); //h
		result.push_back((anchor[2] + anchor[0]) / 2); //ctrx
		result.push_back((anchor[3] + anchor[1]) / 2); //ctry
		return result;
	}

	template <typename Dtype>
	vector<float> RPNLayer<Dtype>::mkanchor(float w, float h, float x_ctr, float y_ctr){
		//given a vector of w and h around a center, output the set of anchors
		vector<float> tmp;
		tmp.push_back(x_ctr - 0.5*(w - 1));
		tmp.push_back(y_ctr - 0.5*(h - 1));
		tmp.push_back(x_ctr + 0.5*(w - 1));
		tmp.push_back(y_ctr + 0.5*(h - 1));
		return tmp;
	}

	template <typename Dtype>
	void RPNLayer<Dtype>::proposal_local_anchor(int now_stride){
		 int length = mymax(map_width_, map_height_);
		//int length = mymax(map_width_/now_stride, map_height_/now_stride); //add by cjs
		//LOG(INFO) << length;
		int step = map_width_ * map_height_;
		int *map_m = new int[length];
		for (int i = 0; i < length; ++i)
		{
			map_m[i] = i * now_stride;
		}
		Dtype *shift_x = new Dtype[step];
		Dtype *shift_y = new Dtype[step];
		for (int i = 0; i < map_height_; ++i)
		{
			for (int j = 0; j < map_width_; ++j)
			{
				shift_x[i*map_width_ + j] = map_m[j];
				shift_y[i*map_width_ + j] = map_m[i];
				//LOG(INFO) <<shift_x[i + j];
				//LOG(INFO) << shift_y[i + j];
			}
		}
		local_anchors_->Reshape(1, anchors_nums_ * 4, map_height_, map_width_);
		Dtype *a = local_anchors_->mutable_cpu_data();
		for (int i = 0; i < anchors_nums_; ++i)
		{
			caffe_set(step, Dtype(anchors_[i * 4 + 0]), a + (i * 4 + 0) *step);
			caffe_set(step, Dtype(anchors_[i * 4 + 1]), a + (i * 4 + 1) *step);
			caffe_set(step, Dtype(anchors_[i * 4 + 2]), a + (i * 4 + 2) *step);
			caffe_set(step, Dtype(anchors_[i * 4 + 3]), a + (i * 4 + 3) *step);
			caffe_axpy(step, Dtype(1), shift_x, a + (i * 4 + 0)*step);
			caffe_axpy(step, Dtype(1), shift_x, a + (i * 4 + 2)*step);
			caffe_axpy(step, Dtype(1), shift_y, a + (i * 4 + 1)*step);
			caffe_axpy(step, Dtype(1), shift_y, a + (i * 4 + 3)*step);
		}
   	 	delete [] map_m;
    	delete [] shift_x;
    	delete [] shift_y;
	}

	template<typename Dtype>
	void RPNLayer<Dtype>::filter_boxs(vector<abox>& aboxes_tmp){
		//Remove all boxes with any side smaller than min_size
		float localMinSize = min_size_*src_scale_;
		aboxes_tmp.clear();
		int map_width = m_box_->width();
		int map_height = m_box_->height();
		int map_channel = m_box_->channels();
		const Dtype *box = m_box_->cpu_data();
		const Dtype *score = m_score_->cpu_data();
 
		int step = 4 * map_height*map_width;
		int one_step = map_height*map_width;
		int offset_w, offset_h, offset_x, offset_y, offset_s;
 
		for (int h = 0; h < map_height; ++h)
		{
			for (int w = 0; w < map_width; ++w)
			{
				offset_x = h*map_width + w;
				offset_y = offset_x + one_step;
				offset_w = offset_y + one_step;
				offset_h = offset_w + one_step;
				offset_s = one_step*anchors_nums_+h*map_width + w;
				for (int c = 0; c < map_channel / 4; ++c)
				{
					Dtype width = box[offset_w], height = box[offset_h];
					if (width < localMinSize || height < localMinSize)
					{
					}
					else
					{
						abox tmp;
						tmp.batch_ind = 0;
						tmp.x1 = box[offset_x] - 0.5*width;
						tmp.y1 = box[offset_y] - 0.5*height;
						tmp.x2 = box[offset_x] + 0.5*width;
						tmp.y2 = box[offset_y] + 0.5*height;
						tmp.x1 = mymin(mymax(tmp.x1, 0), src_width_);
						tmp.y1 = mymin(mymax(tmp.y1, 0), src_height_);
						tmp.x2 = mymin(mymax(tmp.x2, 0), src_width_);
						tmp.y2 = mymin(mymax(tmp.y2, 0), src_height_);
						tmp.score = score[offset_s];
						aboxes_tmp.push_back(tmp);
						
					}
					offset_x += step;
					offset_y += step;
					offset_w += step;
					offset_h += step;
					offset_s += one_step;
				}
			}
		}
	}

	template<typename Dtype>
	void RPNLayer<Dtype>::bbox_tranform_inv(){
		int channel = m_box_->channels();
		int height = m_box_->height();
		int width = m_box_->width();
		int step = height*width;

		Dtype * a = m_box_->mutable_cpu_data();
		Dtype * b = local_anchors_->mutable_cpu_data();

		for (int i = 0; i < channel / 4; ++i)
		{
			caffe_axpy(2*step, Dtype(-1), b + (i * 4 + 0)*step, b + (i * 4 + 2)*step);
			caffe_add_scalar(2 * step, Dtype(1), b + (i * 4 + 2)*step);
			caffe_axpy(2*step, Dtype(0.5), b + (i * 4 + 2)*step, b + (i * 4 + 0)*step);
			caffe_mul(2 * step, b + (i * 4 + 2)*step, a + (i * 4 + 0)*step, a + (i * 4 + 0)*step);
			caffe_add(2 * step, b + (i * 4 + 0)*step, a + (i * 4 + 0)*step, a + (i * 4 + 0)*step);
			caffe_exp(2*step, a + (i * 4 + 2)*step, a + (i * 4 + 2)*step);
			caffe_mul(2 * step, b + (i * 4 + 2)*step, a + (i * 4 + 2)*step, a + (i * 4 + 2)*step);
		}
	}

	template<typename Dtype>
	void RPNLayer<Dtype>::nms(std::vector<abox> &input_boxes, float nms_thresh){
		std::vector<float>vArea(input_boxes.size());
		for (int i = 0; i < input_boxes.size(); ++i)
		{
			vArea[i] = (input_boxes.at(i).x2 - input_boxes.at(i).x1 + 1)
				* (input_boxes.at(i).y2 - input_boxes.at(i).y1 + 1);
		}
		for (int i = 0; i < input_boxes.size(); ++i)
		{
			for (int j = i + 1; j < input_boxes.size();)
			{
				float xx1 = std::max(input_boxes[i].x1, input_boxes[j].x1);
				float yy1 = std::max(input_boxes[i].y1, input_boxes[j].y1);
				float xx2 = std::min(input_boxes[i].x2, input_boxes[j].x2);
				float yy2 = std::min(input_boxes[i].y2, input_boxes[j].y2);
				float w = std::max(float(0), xx2 - xx1 + 1);
				float	h = std::max(float(0), yy2 - yy1 + 1);
				float	inter = w * h;
				float ovr = inter / (vArea[i] + vArea[j] - inter);
				if (ovr >= nms_thresh)
				{
					input_boxes.erase(input_boxes.begin() + j);
					vArea.erase(vArea.begin() + j);
				}
				else
				{
					j++;
				}
			}
		}

	}

	template <typename Dtype>
	void RPNLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom,
		const vector<Blob<Dtype>*>& top){
		vector<abox> aboxes; //fpn add
		//this is where for loop starts
		for(int ind_bottom = 1; ind_bottom < 6; ind_bottom++){
			gen_anchors_.clear();
			generate_anchors(feat_strides_[ind_bottom-1]);
			anchors_nums_ = gen_anchors_.size();
			anchors_ = new int[anchors_nums_ * 4];
			for (int i = 0; i<gen_anchors_.size(); ++i)
			{
				for (int j = 0; j<gen_anchors_[i].size(); ++j)
				{
					anchors_[i*4+j] = gen_anchors_[i][j];
				}
			}
			top[0]->Reshape(1, 5, 1, 1);
			if (top.size() > 1)
			{
				top[1]->Reshape(1, 1, 1, 1);
			}

			//map_width_ = bottom[0]->data_at(0, 1,0,0);
			//map_height_ = bottom[0]->data_at(0, 0,0,0);
			//LOG(INFO) << "map_width_ is " << map_width_;
			//LOG(INFO) << "map_height_ is " << map_height_;
			map_width_ = bottom[ind_bottom]->width();
			map_height_ = bottom[ind_bottom]->height();

			m_box_->CopyFrom(*(bottom[ind_bottom]), false, true);//copy bbox diff from bboxdelta
			m_score_->CopyFrom(*(bottom[ind_bottom+5]),false,true);//copy score from score

			//get info
			src_height_ = bottom[0]->data_at(0, 0,0,0);
			src_width_ = bottom[0]->data_at(0, 1,0,0);
			src_scale_ = bottom[0]->data_at(0, 2, 0, 0);

			proposal_local_anchor(feat_strides_[ind_bottom-1]);
			bbox_tranform_inv();
			vector<abox>aboxes_tmp;
			filter_boxs(aboxes_tmp);
			//fpn add
			for(int iter = 0; iter < aboxes_tmp.size(); iter++){
				aboxes.push_back(aboxes_tmp[iter]);
			}
			delete [] anchors_;
		}
		
		//clock_t start, end;
		//start = clock();

		std::sort(aboxes.rbegin(), aboxes.rend()); //降序
		if (pre_nms_topN_ > 0)
		{
			int tmp = mymin(pre_nms_topN_, aboxes.size());
			aboxes.erase(aboxes.begin() + tmp, aboxes.end());
		}
		nms(aboxes,nms_thresh_);

		//end = clock();
		//std::cout << "sort nms:" << (double)(end - start) / CLOCKS_PER_SEC << std::endl;
		if (post_nms_topN_ > 0)
		{
			int tmp = mymin(post_nms_topN_, aboxes.size());
			aboxes.erase(aboxes.begin() + tmp, aboxes.end());
		}
		top[0]->Reshape(aboxes.size(),5,1,1);
		Dtype *top0 = top[0]->mutable_cpu_data();
		for (int i = 0; i < aboxes.size(); ++i)
		{
			//caffe_copy(aboxes.size() * 5, (Dtype*)aboxes.data(), top0);
			top0[0] = aboxes[i].batch_ind;
			top0[1] = aboxes[i].x1;
			top0[2] = aboxes[i].y1; 
			top0[3] = aboxes[i].x2;
			top0[4] = aboxes[i].y2;
			top0 += top[0]->offset(1);
			//LOG(INFO) << top0[0] << ' , ' << top0[1] << ' , ' << top0[2] << ' , ' << top0[3] << ' , ' << top0[4] << ' , ';
		}
		if (top.size()>1)
		{
			top[1]->Reshape(aboxes.size(), 1,1,1);
			Dtype *top1 = top[1]->mutable_cpu_data();
			for (int i = 0; i < aboxes.size(); ++i)
			{
				top1[0] = aboxes[i].score;
				top1 += top[1]->offset(1);
			}
		}	
	}
#ifdef CPU_ONLY
		STUB_GPU(RPNLayer);
#endif
 
	INSTANTIATE_CLASS(RPNLayer);
	REGISTER_LAYER_CLASS(RPN);
}//namespcae caffe












