#include <thread>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <malloc.h>
void func1()
{
	while (1)
	{
		void *p = malloc(300);
		free(p);
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}
}
void func2()
{
	while (1)
	{
		void *p = malloc(300);
		void * p1 = realloc(p, 6000);
		void* p2 = calloc(1, 7000);
		void* p3 = memalign(4096, 8000);
		void* p4 = valloc(9000);
		void* p5 = pvalloc(10000);
		void* p6 = nullptr;
		posix_memalign(&p6, 4096, 11000);
		static int count = 0;
		printf("count:%d\n",++count);
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}
}

int main()
{
	std::thread thd[10];
	for (int i = 0; i < 10; ++i)
	{
		thd[i] = std::thread(func1);
	}
	std::thread thd2(func2);
	for (int i = 10; i < 20; ++i)
	{
		thd[i].join();
	}
	thd2.join();
	return 0;
}