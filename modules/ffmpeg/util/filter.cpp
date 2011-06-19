#include "../stdafx.h"

#include "filter.h"

#include "../ffmpeg_error.h"
#include "util.h"

#include <common/exception/exceptions.h>
#include <core/producer/frame/basic_frame.h>
#include <core/producer/frame/frame_factory.h>
#include <core/mixer/write_frame.h>

#include <cstdio>
#include <sstream>

extern "C" 
{
	#define __STDC_CONSTANT_MACROS
	#define __STDC_LIMIT_MACROS
	#include <libavutil/avutil.h>
	#include <libavutil/imgutils.h>
	#include <libavfilter/avfilter.h>
	#include <libavfilter/avcodec.h>
	#include <libavfilter/vsrc_buffer.h>
	#include <libavfilter/avfiltergraph.h>
}

namespace caspar {
	
struct filter::implementation
{
	const std::string						filters_;
	std::shared_ptr<AVFilterGraph>			graph_;
	AVFilterContext*						video_in_filter_;
	AVFilterContext*						video_out_filter_;
	std::deque<std::shared_ptr<AVFrame>>	buffer_;
		
	implementation(const std::string& filters) 
		: filters_(filters)
	{}

	void push(const safe_ptr<AVFrame>& frame)
	{		
		int errn = 0;	

		if(!graph_)
		{
			graph_.reset(avfilter_graph_alloc(), [](AVFilterGraph* p){avfilter_graph_free(&p);});
			
			// Input
			std::stringstream buffer_ss;
			buffer_ss << frame->width << ":" << frame->height << ":" << frame->format << ":" << 0 << ":" << 0 << ":" << 0 << ":" << 0; // don't care about pts and aspect_ratio
			errn = avfilter_graph_create_filter(&video_in_filter_, avfilter_get_by_name("buffer"), "src", buffer_ss.str().c_str(), NULL, graph_.get());
			if(errn < 0)
			{
				BOOST_THROW_EXCEPTION(caspar_exception() <<	msg_info(av_error_str(errn)) <<
					boost::errinfo_api_function("avfilter_graph_create_filter") <<	boost::errinfo_errno(AVUNERROR(errn)));
			}

			// Output
			errn = avfilter_graph_create_filter(&video_out_filter_, avfilter_get_by_name("nullsink"), "out", NULL, NULL, graph_.get());
			if(errn < 0)
			{
				BOOST_THROW_EXCEPTION(caspar_exception() <<	msg_info(av_error_str(errn)) <<
					boost::errinfo_api_function("avfilter_graph_create_filter") << boost::errinfo_errno(AVUNERROR(errn)));
			}
			
			AVFilterInOut* outputs = reinterpret_cast<AVFilterInOut*>(av_malloc(sizeof(AVFilterInOut)));
			AVFilterInOut* inputs  = reinterpret_cast<AVFilterInOut*>(av_malloc(sizeof(AVFilterInOut)));

			outputs->name			= av_strdup("in");
			outputs->filter_ctx		= video_in_filter_;
			outputs->pad_idx		= 0;
			outputs->next			= NULL;

			inputs->name			= av_strdup("out");
			inputs->filter_ctx		= video_out_filter_;
			inputs->pad_idx			= 0;
			inputs->next			= NULL;
			
			errn = avfilter_graph_parse(graph_.get(), filters_.c_str(), inputs, outputs, NULL);
			if(errn < 0)
			{
				BOOST_THROW_EXCEPTION(caspar_exception() <<	msg_info(av_error_str(errn)) <<
					boost::errinfo_api_function("avfilter_graph_parse") << boost::errinfo_errno(AVUNERROR(errn)));
			}

//			av_free(outputs);
//			av_free(inputs);

			errn = avfilter_graph_config(graph_.get(), NULL);
			if(errn < 0)
			{
				BOOST_THROW_EXCEPTION(caspar_exception() <<	msg_info(av_error_str(errn)) 
					<<	boost::errinfo_api_function("avfilter_graph_config") <<	boost::errinfo_errno(AVUNERROR(errn)));
			}
		}
	
		errn = av_vsrc_buffer_add_frame(video_in_filter_, frame.get(), 0);
		if(errn < 0)
		{
			BOOST_THROW_EXCEPTION(caspar_exception() << msg_info(av_error_str(errn)) <<
				boost::errinfo_api_function("av_vsrc_buffer_add_frame") << boost::errinfo_errno(AVUNERROR(errn)));
		}

		errn = avfilter_poll_frame(video_out_filter_->inputs[0]);
		if(errn < 0)
		{
			BOOST_THROW_EXCEPTION(caspar_exception() <<	msg_info(av_error_str(errn)) <<
				boost::errinfo_api_function("avfilter_poll_frame") << boost::errinfo_errno(AVUNERROR(errn)));
		}

		std::generate_n(std::back_inserter(buffer_), errn, [&]{return get_frame();});
	}
		
	std::shared_ptr<AVFrame> get_frame()
	{		
		auto link = video_out_filter_->inputs[0];

		int errn = avfilter_request_frame(link); 			
		if(errn < 0)
		{
			BOOST_THROW_EXCEPTION(caspar_exception() <<	msg_info(av_error_str(errn)) <<
				boost::errinfo_api_function("avfilter_request_frame") << boost::errinfo_errno(AVUNERROR(errn)));
		}
		
		auto pic   = reinterpret_cast<AVPicture*>(link->cur_buf->buf);
		
		std::shared_ptr<AVFrame> frame(avcodec_alloc_frame(), av_free);
		avcodec_get_frame_defaults(frame.get());	

		for(size_t n = 0; n < 4; ++n)
		{
			frame->data[n]		= pic->data[n];
			frame->linesize[n]	= pic->linesize[n];
		}

		frame->width	= link->w;
		frame->height	= link->h;
		frame->format	= link->format;

		return frame;
	}
	
	bool try_pop(std::shared_ptr<AVFrame>& frame)
	{
		if(buffer_.empty())
			return false;

		frame = buffer_.front();
		buffer_.pop_front();

		return true;
	}
};

filter::filter(const std::string& filters) : impl_(new implementation(filters)){}
void filter::push(const safe_ptr<AVFrame>& frame) {return impl_->push(frame);}
bool filter::try_pop(std::shared_ptr<AVFrame>& frame){return impl_->try_pop(frame);}
size_t filter::size() const {return impl_->buffer_.size();}

}