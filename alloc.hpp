#ifndef _ALLOC_HPP
#define _ALLOC_HPP

namespace project
{
	//自定义线性内存分配器
	template<typename T>
	class CustomizedAllocator
	{
	public:
		//分配器模板值类型
		using value_type = T;
		//分配器模板大小类型
		using size_type = std::size_t;
		//分配器模板比较类型
		using difference_type = std::ptrdiff_t;

		//构造函数
		//参数：最大分配大小
		CustomizedAllocator(size_type max_size) :max_size_(max_size), allocated_(new size_type(0)), count_(new size_type(1)) { mem_ = ::operator new[](max_size_ * sizeof(T)); }
		//拷贝构造函数
		CustomizedAllocator(const CustomizedAllocator& other)
		{
			mem_ = other.mem_;
			max_size_ = other.max_size_;
			allocated_ = other.allocated_;
			count_ = other.count_;
			++(*count_);
		}
		//拷贝赋值运算符
		CustomizedAllocator& operator=(const CustomizedAllocator& other)
		{
			if (this != &other)
			{
				mem_ = other.mem_;
				max_size_ = other.max_size_;
				allocated_ = other.allocated_;
				count_ = other.count_;
				++(*count_);
			}
			return *this;
		}
		//移动构造函数
		CustomizedAllocator(CustomizedAllocator&& other) noexcept :mem_(other.mem_), max_size_(other.max_size_), allocated_(other.allocated_), count_(other.count_)
		{
			other.mem_ = nullptr;
			other.max_size_ = 0;
			other.allocated_ = nullptr;
			other.count_ = nullptr;
		}
		//移动赋值运算符
		CustomizedAllocator& operator=(CustomizedAllocator&& other)
		{
			if (this != &other)
			{
				mem_ = other.mem_;
				max_size_ = other.max_size_;
				allocated_ = other.allocated_;
				count_ = other.count_;
				other.mem_ = nullptr;
				other.max_size_ = 0;
				other.allocated_ = nullptr;
				other.count_ = nullptr;
			}
			return *this;
		}
		//模板构造函数，允许从其他类型实例模板转化
		template<typename U>CustomizedAllocator(const CustomizedAllocator<U>& other) :mem_(other.mem_), max_size_(other.max_size_), allocated_(other.allocated_), count_(other.count_) { ++(*count_); }
		//析构函数
		~CustomizedAllocator()
		{
			release();
		}
		//分配（线性）空间
		T* allocate(size_type n) noexcept
		{
			if (n == 0 || n < 0)
				return nullptr;
			if (n > max_size_ - *allocated_)
				throw std::runtime_error("Demand allocation space is larger than the free.");
			T* ret = static_cast<T*>(mem_) + *allocated_;
			*allocated_ += n;
			return ret;
		}
		//收回（线性）空间
		void deallocate(T* ptr, size_type n) noexcept
		{
			if (n > max_size_ || n > *allocated_)
				return;
			*allocated_ -= n;
		}

		template<typename U>bool operator==(const CustomizedAllocator<U>& other)const { return mem_ == other.mem_ && max_size_ == other.max_size_ && allocated_ == other.allocated_; }
		template<typename U>bool operator!=(const CustomizedAllocator<U>& other)const { return mem_ != other.mem_ || max_size_ != other.max_size_ || allocated_ != other.allocated_; }

	private:
		//类型无关指针
		void* mem_ = nullptr;
		//分配大小
		size_type* allocated_ = nullptr;
		//共享计数
		size_type* count_ = nullptr;
		//最大分配大小
		size_type max_size_ = 0;
		//声明其他实例模板类为友元
		template<typename U>
		friend class CustomizedAllocator;
		//释放资源
		void release()
		{
			if (mem_)
				if (count_)
				{
					if (*count_)
						--(*count_);
					if (!*count_)
					{
						//当共享计数为0时，释放所有资源
						::operator delete[](mem_);
						delete count_;
						if (allocated_)
							delete allocated_;
						mem_ = nullptr;
						count_ = nullptr;
						allocated_ = nullptr;
					}
				}
		}
	};
}

#endif