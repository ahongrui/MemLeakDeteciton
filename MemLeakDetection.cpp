#include <execinfo.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <errno.h>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <time.h>
#include <signal.h>
#include <malloc.h>
#include <sys/mman.h>
#define PRT printf
#define TABLE_SIZE	98317UL
#define MAX_STACK_DEPTH 32
#define RPT_SIGNAL 64
#define DEF_PAGE_SIZE 4096
#define DEF_OUTPUT_PATH 	"./"

#if (define (__x86_64__) || define (__aarch64__))
#define FLAG_VALUE 0XFFFFFFFFFFFFFFFF
#else
#define FLAG_VALUE 0XFFFFFFFF
#endif

extern "C" void* __libc_malloc(size_t bytes);
extern "C" void* __libc_free(void* mem);
extern "C" void* __libc_calloc(size_t n, size_t elem_size);
extern "C" void* __libc_realloc(void* __ptr, size_t __size);
extern "C" void* __libc_memalign(size_t __alignment, size_t __size);
// 内存操作函数
typedef void* (*FUNC_MALLOC)(unsigned int size);
typedef void* (*FUNC_CALLOC)(size_t num, size_t size);
typedef void* (*FUNC_REALLOC)(void *mem_address, size_t newsize);
typedef void* (*FUNC_VALLOC)(size_t size);
typedef void* (*FUNC_PVALLOC)(size_t size);
typedef int (*FUNC_POSIX_MEMALIGN)(void **memptr, size_t alignment, size_t size);
typedef void* (*FUNC_MEMALIGN)(size_t boundary, size_t size);
typedef void* (*FUNC_ALIGNED_ALLOC)(size_t alignment, size_t size);
typedef void (*FUNC_FREE)(void *ptr);
static FUNC_MALLOC g_pfnMalloc = NULL;
static FUNC_CALLOC g_pfnCalloc = NULL;
static FUNC_REALLOC g_pfnRealloc = NULL;
static FUNC_VALLOC g_pfnValloc = NULL;
static FUNC_PVALLOC g_pfnPvalloc = NULL;
static FUNC_POSIX_MEMALIGN g_pfnPosixMemalign = NULL;
static FUNC_MEMALIGN g_pfnMemalign = NULL;
static FUNC_ALIGNED_ALLOC g_pfnAlignAlloc = NULL;
static FUNC_FREE g_pfnFree = NULL;

// 定义backtrace函数
static int(*g_pfnBackTrace)(void** stack, int size);

// 完成初始化前malloc、free不做业务统计
static bool g_bInitFlag = false;

// 防止内存操作函数死循环
thread_local static int g_tlsKey = 0;

//堆栈信息结构体
struct BtInfo
{
	struct BtInfo* next;			//单向链表
	int		requestLength;			//用户申请的内存大小
	int		extraLength;			//额外在用户内存前面扩展的内存长度
	int		count;					//栈调用个数
	int		depth;					//栈深度
	size_t hash;					//hash
	void* bt[MAX_STACK_DEPTH];		//依次的栈帧地址
};
//内存前置结构体
struct PtrRep
{
	BtInfo* stack;
	size_t  flag;
};

//哈希表存储堆栈信息
static BtInfo* g_hashTable[TABLE_SIZE] = { 0 };

/*  gcc内置函数 */
/***********************************************/
#define GCCBT(i) \
    if(i >= size) return i;\
    frameNow = __builtin_frame_address(i);\
    if((unsigned long)frameNow <= (unsigned long)frameLast ||\
    ((unsigned long)frameLast != 0 && (unsigned long)frameNow > (unsigned long)frameLast + (1ULL<<24)))\
        return i;\
    frameLast = frameNow;\
    stack[i] = __builtin_extract_return_addr(__builtin_return_address(i));\

static int gccBacktrace(void** stack, int size)
{
	void* frameNow = 0;
	void* frameLast = 0;
	GCCBT(0); GCCBT(1); GCCBT(2); GCCBT(3); GCCBT(4); GCCBT(5); GCCBT(6); GCCBT(7);
	GCCBT(8); GCCBT(9); GCCBT(10); GCCBT(11); GCCBT(12); GCCBT(13); GCCBT(14); GCCBT(15);
	GCCBT(16); GCCBT(17); GCCBT(18); GCCBT(19); GCCBT(20); GCCBT(21); GCCBT(22); GCCBT(23);
	GCCBT(24); GCCBT(25); GCCBT(26); GCCBT(27); GCCBT(28); GCCBT(29); GCCBT(30); GCCBT(31);
	return 32;
}

/*  内置汇编获取寄存器返回值 */
/***********************************************/
static int asmBacktrace(void** stack, int size)
{
#ifndef __x86_64__
	return 0;
#else
	int frame = 0;
	void ** ebp;
	unsigned long long func_frame_distance = 0;
	__asm__ __volatile__("mov %%rbp, %0;\n\t" : "=m"(ebp) ::"memory");
	while (ebp && frame < size
		&& (unsigned long long)(*ebp) < (unsigned long long)(ebp) + (1ULL << 24)//16M
		&& (unsigned long long)(*ebp) > (unsigned long long)(ebp))
	{
		stack[frame++] = *(ebp + 1);
		ebp = (void**)(*ebp);
	}
	return frame;
#endif
}

// 初始化函数
static void InitMemFunc()
{
	g_pfnMalloc = (FUNC_MALLOC)dlsym(RTLD_NEXT, "malloc");
	g_pfnFree = (FUNC_FREE)dlsym(RTLD_NEXT, "free");
	g_pfnCalloc = (FUNC_CALLOC)dlsym(RTLD_NEXT, "calloc");
	g_pfnRealloc = (FUNC_REALLOC)dlsym(RTLD_NEXT, "realloc");
	g_pfnValloc = (FUNC_VALLOC)dlsym(RTLD_NEXT, "valloc");
	g_pfnPvalloc = (FUNC_PVALLOC)dlsym(RTLD_NEXT, "pvalloc");
	g_pfnMemalign = (FUNC_MEMALIGN)dlsym(RTLD_NEXT, "memalign");
	g_pfnPosixMemalign = (FUNC_POSIX_MEMALIGN)dlsym(RTLD_NEXT, "posix_memalign");
	g_pfnAlignAlloc = (FUNC_ALIGNED_ALLOC)dlsym(RTLD_NEXT, "aligned_alloc");
}

// 配置
static int	g_maxStackDepth = MAX_STACK_DEPTH; 	//bt栈深度
static char g_btType[10] = "unw";				//bt方式：libc/gcc/asm/unw
static int 	g_rptSignal = RPT_SIGNAL;			//用户通知信号
static char g_rptPath[1024] = {0};				//默认输出路径
static char g_rptFile[128] = { 0 };				//输出文件名称
static char g_rptExePath[1024] = { 0 };			//执行文件绝对路径
static char g_libunwindPath[1024] = {0}; 		//libunwind.so路径
static size_t g_pageSize = DEF_PAGE_SIZE;		//内存对齐页面默认值

static void InitConfig()
{
	//页面size
	int tmpPageSize = getpagesize();
	g_pageSize = tmpPageSize > 0 ? tmpPageSize : g_pageSize;
	
	//堆栈深度
	char* strEnv = getenv("MAL_MAX_STACK_DEPTH");
	if(strEnv != NULL && strEnv[0] != '\0')
	{
		int iDepth = atoi(strEnv);
		g_maxStackDepth = (iDepth > 0 && iDepth < MAX_STACK_DEPTH) ? iDepth :  g_maxStackDepth;
	}
	
	//bt方式
	strEnv = getenv("MAL_BACKTRACE_TYPE");
	if(strEnv != NULL && strEnv[0] != '\0')
	{
		strncpy(g_btType, strEnv, sizeof(g_btType) - 1);
	}
	
	//通知信号
	strEnv = getenv("MAL_REPORT_SIGNAL");
	if(strEnv != NULL && strEnv[0] != '\0')
	{
		int iSignal = atoi(strEnv);
		g_rptSignal = (iSignal > 0 && iSignal <= 64) ? iSignal :  g_rptSignal;
	}
	
	//输出路径
	strEnv = getenv("MAL_REPORT_PATH");
	if(strEnv != NULL && strEnv[0] != '\0')
	{
		strncpy(g_rptPath, strEnv, sizeof(g_rptPath) - 1);
	}
	else
	{
		strncpy(g_rptPath, DEF_OUTPUT_PATH, sizeof(g_rptPath) - 1);
	}

	//输出文件名称
	readlink("/proc/self/exe", g_rptExePath, sizeof(g_rptExePath) - 1);
	int length = strlen(g_rptExePath) - 1;
	while (g_rptExePath[length] != '/' && length > 0)
	{
		--length;
	}
	snprintf(g_rptFile, sizeof(g_rptFile), "pid_%llu_proc_%s.bt",getpid(), (char*)(g_rptExePath+length+1));
	
	//libunwind.so加载路径
	strEnv = getenv("MAL_LIBUNWIND_PATH");
	if(strEnv != NULL && strEnv[0] != '\0')
	{
		strncpy(g_libunwindPath, strEnv, sizeof(g_libunwindPath) - 1);
	}
	else
	{
		strncpy(g_libunwindPath, "./libunwind.so", sizeof(g_libunwindPath) - 1);
	}
}

static void InitBtFunc()
{
	if (strcmp(g_btType, "gcc") == 0)
	{
		g_pfnBackTrace = gccBacktrace;
	}
	else if (strcmp(g_btType, "libc") == 0)
	{
		g_pfnBackTrace = backtrace;
	}	
	else if (strcmp(g_btType, "asm") == 0)
	{
		g_pfnBackTrace = asmBacktrace;
	}	
	else if (strcmp(g_btType, "unw") == 0)
	{
		void* handle = dlopen(g_libunwindPath, RTLD_NOW|RTLD_LOCAL|RTLD_DEEPBIND);
		if (handle == nullptr)
		{
			g_pfnBackTrace = gccBacktrace;
			PRT("dlopen failed, %s\n",dlerror());
			return;
		}
		g_pfnBackTrace = (int(*)(void**, int))dlsym(handle, "unw_backtrace");
		if (g_pfnBackTrace == nullptr)
		{
			g_pfnBackTrace = gccBacktrace;
			PRT("dlsym failed, %s\n", dlerror());
			return;
		}
	}	
	else
	{
		g_pfnBackTrace = gccBacktrace; //默认使用backtrace
	}
}

static bool IsExeFile(const char* fname)
{
	bool bRet = true;
	int lenSoName = strlen(fname);
	int lenExePath = strlen(g_rptExePath);
	while (lenSoName-- >= 0 && lenExePath-- >= 0)
	{
		if (g_rptExePath[lenExePath] == '/' || fname[lenSoName] == '/')
		{
			break;
		}
		if (g_rptExePath[lenExePath] != fname[lenSoName])
		{
			bRet = false;
			break;
		}
	}
	return bRet;
}

//写入单个栈的bt统计信息
static void RecordSingleStack(BtInfo* bp, FILE* fp)
{
	fprintf(fp,"count:%d size:%d", bp->count, bp->requestLength);
	for (int i = 0; i < bp->depth; ++i)
	{
		void* ptr = bp->bt[i];
		Dl_info info = {0};
		if (dladdr(ptr, &info) == 0)
		{
			fprintf(fp, " unknow:%p", ptr);
			continue;
		}
		size_t iAddr = 0;
		if (IsExeFile(info.dli_fname))
		{
			iAddr = (size_t)ptr;
		}
		else
		{	//如果是共享库，要减去偏移
			iAddr = (size_t)ptr - (size_t)info.dli_fbase;
		}
		fprintf(fp, " %s:0x%x", (info.dli_fname != 0 && *info.dli_fname != 0 ? info.dli_fname:"unknow"), iAddr);
	}
	fprintf(fp, "\n");
}

// 统计当前所有内存申请的堆栈信息
static void StaticStackInfo()
{
	//获取当前时间
	time_t now = time(0);
	struct tm cur = { 0 };
	localtime_r(&now, &cur);
	char timeNow[64] = { 0 };
	snprintf(timeNow, sizeof(timeNow) - 1, "time_%04d-%02d-%02dT%02d%02d%02d",
		cur.tm_year+1900, cur.tm_mon+1,cur.tm_mday, cur.tm_hour, cur.tm_min, cur.tm_sec);
	//打开写入文件
	char tmpFile[1024] = { 0 };
	snprintf(tmpFile, sizeof(tmpFile) -1, "%s/%s_%s",g_rptPath, timeNow, g_rptFile);
	FILE* fp = fopen(tmpFile, "wt+");
	if (fp == nullptr)
	{
		PRT("open file:%s failed,err:%s\n",tmpFile, strerror(errno));
		return;
	}
	fprintf(fp, "%s\n", g_rptExePath);
	//遍历hashtable写入堆栈
	for (int i = 0; i < TABLE_SIZE; ++i)
	{
		for (BtInfo* tmp = g_hashTable[i]; tmp != nullptr; tmp = tmp->next)
		{
			if (tmp->count <= 0)
			{
				continue;
			}
			RecordSingleStack(tmp, fp);
		}
	}
	fclose(fp);
}

// 信号处理函数
static void SigHandle(int sigNum)
{
	std::thread thd(StaticStackInfo);
	thd.detach();
}

//  __attribute__((constructor)) -- 该函数在main函数之前被执行，完成初始化
__attribute__((constructor)) static void InitAll() 
{
	static int g_init = 0;
	if (g_init == 1)
	{
		return;
	}
	g_init = 1;
	InitMemFunc();
	InitConfig();
	InitBtFunc();
	void(*sign)(int);
	sign = signal(g_rptSignal, SigHandle);
	if (sign == SIG_ERR)
	{
		PRT("signal failed, signNum:%d\n", g_rptSignal);
	}
	g_bInitFlag = true;
}

//根据栈帧地址获取key值
static size_t GetHashValue(void** stack, int size)
{
	size_t seed = 0;
	for (int i = 0; i < size; ++i)
	{
		seed ^= std::hash<void*>()(stack[i]) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
	}
	return seed;
}
// 判断是否是同一个栈
static bool IsEqual(const BtInfo* ptr1, const BtInfo* ptr2)
{
	if (ptr1->hash != ptr2->hash)
	{
		return false;
	}
	if (ptr1->depth != ptr2->depth)
	{
		return false;
	}
	for (int i = 0; i < ptr1->depth; ++i)
	{
		if (ptr1->bt[i] != ptr2->bt[i])
		{
			return false;
		}
	}
	return true;
}

// 将申请内存的栈信息存入hashtable中
// ptr -- 真实申请的内存地址  == 额外扩展内存+返回用户内存
//totalLength: 实际总长度 
//extraLength: totalLength - 用户长度
static void AddStackInfo(void* ptr, int totalLength, int extraLength)
{
	PtrRep* pRep = (PtrRep*)((char*)ptr + extraLength - sizeof(PtrRep));
	pRep->flag = FLAG_VALUE;
	BtInfo* cur = new BtInfo;
	cur->next = nullptr;
	cur->requestLength = totalLength - extraLength;
	cur->extraLength = extraLength;
	cur->count = 1;
	cur->depth = g_pfnBackTrace(cur->bt, g_maxStackDepth);
	cur->hash = GetHashValue(cur->bt, cur->depth);
	int index = cur->hash % TABLE_SIZE;
	//如果数组索引为空，替换为cur，并返回原先的nullptr
	BtInfo* tmp = (BtInfo*)__sync_val_compare_and_swap(&g_hashTable[index],nullptr,cur);
	if (tmp == nullptr)
	{//第一次插入
		pRep->stack = cur;
		return;
	}
	while (true)
	{	
		if (IsEqual(tmp, cur))
		{//相等则计数加一
			__sync_add_and_fetch(&(tmp->count), 1);
			pRep->stack = tmp;
			delete cur;
			return;
		}
		// tmp指向链表下一个
		tmp = (BtInfo*)__sync_val_compare_and_swap(&tmp->next, nullptr, cur);
		if(tmp == nullptr)
		{//已经插入链表尾
			pRep->stack = cur;
			return;
		}
	}
	return;
}

//hashtable里的申请内存的栈信息应用计数减一
//ptr: 入参：用户指针 出参：malloc实际地址
static void DelStackInfo(void* &ptr)
{
	PtrRep* pRep = (PtrRep*)((char*)ptr - sizeof(PtrRep));
	int exterLength = pRep->stack->extraLength;
	__sync_sub_and_fetch(&pRep->stack->count,1);
	ptr = (void*)((char*)ptr - exterLength);
}
void* malloc(size_t bytes)
{
	InitAll();
	if (!g_bInitFlag)
	{
		return g_pfnMalloc(bytes);
	}
	if (g_tlsKey == 1)
	{
		return g_pfnMalloc(bytes);
	}
	g_tlsKey = 1;
	int extraLength = sizeof(PtrRep);
	bytes += extraLength;
	void* pRet = malloc(bytes);
	if (pRet != nullptr)
	{
		AddStackInfo(pRet, bytes, extraLength);
		pRet = (void*)((char*)pRet + extraLength);
	}
	g_tlsKey = 0;
	return pRet;
}

void free(void* mem)
{
	InitAll();
	if (!g_bInitFlag)
	{
		g_pfnFree(mem);
		return;
	}
	if (mem == nullptr)
	{
		return;
	}
	if (g_tlsKey == 1)
	{
		g_pfnFree(mem);
		return;
	}
	size_t flag = *(size_t*)((char*)mem - sizeof(size_t));
	if (flag != FLAG_VALUE)// 判断是否为mla封装函数申请的内存
	{
		g_pfnFree(mem);
		return;
	}
	g_tlsKey = 1;
	DelStackInfo(mem);
	free(mem);
	g_tlsKey = 0;
}

void* calloc(size_t cnt, size_t size)
{
	InitAll();
	if (!g_bInitFlag)
	{
		return g_pfnCalloc == nullptr? nullptr:g_pfnCalloc(cnt, size);
	}
	size_t bytes = cnt*size; // 防止overflow
	if (size != 0 && bytes / size != cnt)
	{
		return 0;
	}
	if (g_tlsKey == 1)
	{
		return g_pfnCalloc(cnt, size);
	}
	g_tlsKey = 1;
	int extraLength = sizeof(PtrRep);
	bytes += extraLength;
	void* pRet = calloc(bytes, 1);
	if (pRet != nullptr)
	{
		AddStackInfo(pRet, bytes, extraLength);
		pRet = (void*)((char*)pRet + extraLength);
	}
	g_tlsKey = 0;
	return pRet;
}

void* realloc(void* ptr, size_t size)
{
	InitAll();
	if (!g_bInitFlag)
	{
		return g_pfnRealloc(ptr, size);
	}
	if (size == 0 && ptr != nullptr)
	{
		free(ptr);
		return nullptr;
	}
	if (ptr == nullptr)
	{
		return malloc(size);
	}
	if (g_tlsKey == 1)
	{
		return g_pfnRealloc(ptr, size);
	}
	g_tlsKey = 1;
	PtrRep* pRep = (PtrRep*)((char*)ptr - sizeof(PtrRep));
	BtInfo* stack = pRep->stack;
	void* newPtr = (void*)((char*)ptr - pRep->stack->extraLength);
	int extraLength = sizeof(PtrRep);
	size += extraLength;
	void* pRet = realloc(newPtr, size);
	if (pRet != nullptr)
	{
		__sync_sub_and_fetch(&stack->count, 1);
		AddStackInfo(pRet, size, extraLength);
		pRet = (void*)((char*)pRet + extraLength);
	}
	g_tlsKey = 0;
	return pRet;
}  

void* memalign(size_t alignment, size_t size)
{
	InitAll();
	if (!g_bInitFlag)
	{
		return g_pfnMemalign(alignment, size);
	}
	if (g_tlsKey == 1)
	{
		return g_pfnMemalign(alignment, size);
	}
	g_tlsKey = 1;
	int extraLength = alignment;
	if (alignment < sizeof(PtrRep))
	{
		extraLength = (sizeof(PtrRep) / alignment + 1) * sizeof(PtrRep);
	}
	size += extraLength;
	void* pRet = memalign(alignment, size);
	if (pRet != nullptr)
	{
		AddStackInfo(pRet, size, extraLength);
		pRet = (void*)((char*)pRet + extraLength);
	}
	g_tlsKey = 0;
	return pRet;
}

void* valloc(size_t size)
{
	return memalign(g_pageSize, size);
}

void* pvalloc(size_t size)
{
	size_t roundedBytes = (size + g_pageSize - 1)&~(g_pageSize - 1);
	return memalign(g_pageSize, roundedBytes);
}

int posix_memalign(void** ptr, size_t alignment, size_t size)
{
	InitAll();
	if (!g_bInitFlag)
	{
		return g_pfnPosixMemalign(ptr, alignment, size);
	}
	if (g_tlsKey == 1)
	{
		return g_pfnPosixMemalign(ptr, alignment, size);
	}
	g_tlsKey = 1;
	int extraLength = alignment;
	if (alignment < sizeof(PtrRep))
	{
		extraLength = (sizeof(PtrRep) / alignment + 1) * sizeof(PtrRep);
	}
	size += extraLength;
	void* pRet = nullptr;
	int iRet = posix_memalign(&pRet, alignment, size);
	if (iRet == 0 && pRet != nullptr)
	{
		AddStackInfo(pRet, size, extraLength);
		pRet = (void*)((char*)pRet + extraLength);
	}
	g_tlsKey = 0;
	*ptr = pRet;
	return iRet;
}

//c11
void* aligned_alloc(size_t alignment, size_t size)
{
	return memalign(alignment, size);
}
















