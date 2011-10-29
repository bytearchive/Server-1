#pragma once

#include "../memory/safe_ptr.h"

#include <concrt.h>
#include <concurrent_queue.h>
#include <tbb/atomic.h>

#include <boost/noncopyable.hpp>

#include <vector>

namespace caspar {
		
#undef Yield

typedef std::vector<std::shared_ptr<void>> ticket_t;

class governor : boost::noncopyable
{
	struct impl : std::enable_shared_from_this<impl>
	{
		tbb::atomic<int> count_;
		tbb::atomic<int> is_running_;
		Concurrency::concurrent_queue<Concurrency::Context*> waiting_contexts_;

		void acquire_ticket()
		{
			if(!is_running_)
				return;

			if(count_ < 1)
				Concurrency::Context::Yield();

			if (--count_ < 0)
			{
				auto context = Concurrency::Context::CurrentContext();
				waiting_contexts_.push(context);
				context->Block();
			}
		}
		
		bool try_acquire_ticket()
		{
			if(!is_running_)
				return false;

			if(count_ < 1)
				Concurrency::Context::Yield();

			if (--count_ < 0)
			{
				++count_;
				return false;
			}
			return true;
		}

		void release_ticket()
		{
			if(!is_running_)
				return;

			if(++count_ <= 0)
			{
				Concurrency:: Context* waiting = NULL;
				while(!waiting_contexts_.try_pop(waiting))
					Concurrency::Context::Yield();
				waiting->Unblock();
			}
		}

	public:

		impl(size_t capacity) 
		{
			is_running_ = true;
			count_ = capacity;
		}
	
		ticket_t acquire()
		{
			acquire_ticket();
		
			auto self = shared_from_this();
			ticket_t ticket;
			ticket.push_back(std::shared_ptr<void>(nullptr, [self](void*)
			{
				self->release_ticket();
			}));
			return ticket;
		}
		
		bool try_acquire(ticket_t& ticket)
		{
			if(!try_acquire_ticket())
				return false;
		
			auto self = shared_from_this();
			ticket.push_back(std::shared_ptr<void>(nullptr, [=](void*)
			{
				self->release_ticket();
			}));

			return true;
		}

		void cancel()
		{
			is_running_ = false;
			Concurrency::Context* waiting = NULL;
			while(waiting_contexts_.try_pop(waiting))
				waiting->Unblock();
		}
	};

public:
	governor(size_t capacity) : impl_(new impl(capacity))
	{
	}
	
	ticket_t acquire()
	{
		return impl_->acquire();
	}

	bool try_acquire(ticket_t& ticket)
	{
		return impl_->try_acquire(ticket);
	}

	void cancel()
	{
		impl_->cancel();
	}

private:
	safe_ptr<impl> impl_;

};

}