#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <sys/time.h>
#define SO_NAME "libmla.so"
static std::string g_path;
static size_t g_count = 0;
static size_t g_total = 0;

class StaticInfo
{
public:
	size_t count;
	size_t total;
	std::vector<std::string> vecBt;
};
std::vector<StaticInfo> g_vecStaticInfos;
static void SplitString(const char* buf, char c, std::vector<std::string>& vec)
{
	std::string str = buf;
	int size = str.size();
	size_t pos = 0;
	size_t lastPos = 0;
	for (; pos < size; ++pos)
	{
		if (str[pos] == c)
		{
			vec.push_back(str.substr(lastPos, pos - lastPos));
			lastPos = pos + 1;
		}
	}
	if (lastPos < size)
	{
		vec.push_back(str.substr(lastPos));
	}
}

static bool Compare(const StaticInfo& bt1, const StaticInfo& bt2)
{
	return bt1.total > bt2.total;
}

static std::string BtAddr2Line(std::string& strSoPath, std::string& strAddr)
{
	std::string cmd = "addr2line -Cfpie " + strSoPath + " " + strAddr + " 2>/dev/null";
	FILE* pp = popen(cmd.c_str(), "r");
	if (pp == nullptr)
	{
		printf("popen failed\n");
		return "\n";
	}
	char buf[10240] = { 0 };
	fread(buf, 1, sizeof(buf), pp);
	pclose(pp);
	return std::string(buf);
}
static int64_t GetTimeTick()
{
	struct timespec spec;
	memset(&spec, 0x0, sizeof(timespec));
	clock_gettime(CLOCK_MONOTONIC,&spec);
	return (int64_t)((spec.tv_sec*1000)+(spec.tv_nsec+1000000/2)/1000000);
}
void LoadBtFile(char* btFile, std::vector<StaticInfo>& vecStaticInfo)
{
	//先读取bt文件
	std::ifstream iFile(btFile);
	if (!iFile.is_open())
	{
		printf("input file open failed\n");
		return;
	}
	char buf[10240] = { 0 };
	int firstLine = 1;
	while (iFile.getline(buf, sizeof(buf)))
	{
		if (firstLine)
		{
			firstLine = 0;
			g_path = buf;
			memset(buf, 0, sizeof(buf));
			continue;
		}
		std::vector<std::string> vecStr;
		SplitString(buf, ' ', vecStr);
		StaticInfo tmpStaticInfo;
		for (int i = 0; i < vecStr.size(); ++i)
		{
			std::string & tmp = vecStr[i];
			if (tmp.find("count") != std::string::npos)
			{
				tmpStaticInfo.count = atoi(tmp.substr(6).c_str());
				//g_count += tmpStaticInfo.count;
				continue;
			}
			if (tmp.find("size") != std::string::npos)
			{
				tmpStaticInfo.total = tmpStaticInfo.count * atoi(tmp.substr(5).c_str());
				//g_total += tmpStaticInfo.total;
				continue;
			}
			tmpStaticInfo.vecBt.push_back(tmp);
		}
		vecStaticInfo.push_back(tmpStaticInfo);
		memset(buf, 0, sizeof(buf));
	}
}

static void BtDiff(char* btFile1, char* btFile2, char* txtFile)
{
	printf("time:%ld, start transition...\n",GetTimeTick());
	std::vector<StaticInfo> vecStaticInfo_1;
	std::vector<StaticInfo> vecStaticInfo_2;
	LoadBtFile(btFile1, vecStaticInfo_1);
	LoadBtFile(btFile2, vecStaticInfo_2);
	printf("time:%ld, complete transition, compare differences...\n",GetTimeTick());
	for (int i = 0; i < vecStaticInfo_2.size(); ++i)
	{
		for (int j = 0; j < vecStaticInfo_1.size(); ++j)
		{
			if (vecStaticInfo_1[j].count == 0)
			{
				continue;
			}
			if (vecStaticInfo_1[j].vecBt.size() != vecStaticInfo_2[i].vecBt.size())
			{
				continue;
			}
			bool bEqual = true;
			for (int k = 0; k < vecStaticInfo_1[j].vecBt.size(); ++k)
			{
				if (vecStaticInfo_1[j].vecBt[k] != vecStaticInfo_2[i].vecBt[k])
				{
					bEqual = false;
					break;
				}
			}
			if(!bEqual) continue;
			if(vecStaticInfo_2[i].count <= vecStaticInfo_1[j].count)
			{
				vecStaticInfo_2[i].count = 0;
			}
			else
			{
				vecStaticInfo_2[i].count -= vecStaticInfo_1[j].count;
			}
			if(vecStaticInfo_2[i].total <= vecStaticInfo_1[j].total)	
			{
				vecStaticInfo_2[i].total = 0;
			}
			else
			{
				vecStaticInfo_2[i].total -= vecStaticInfo_1[j].total;
			}
			
			g_count += vecStaticInfo_2[i].count;
			g_total += vecStaticInfo_2[i].total;
			vecStaticInfo_1[j].count = 0;
			break;
		}
	}
	printf("time:%ld, complete differences, input file...\n",GetTimeTick());
	std::sort(vecStaticInfo_2.begin(), vecStaticInfo_2.end(), Compare);
	//写入输出文件
	std::ofstream oFile(txtFile, std::ios_base::trunc);
	if (!oFile.is_open())
	{
		printf("output file open failed\n");
		return;
	}
	oFile << btFile1 << std::endl;
	oFile << btFile2 << std::endl;
	oFile << "total: " << g_total << "  count: " << g_count << std::endl << std::endl;
	for (int i = 0; i < vecStaticInfo_2.size(); ++i)
	{
		StaticInfo& tmpStaticInfo = vecStaticInfo_2[i];
		if (tmpStaticInfo.total == 0 || tmpStaticInfo.count == 0)
		{
			continue;
		}
		double totalPercent = 100.0*tmpStaticInfo.total / g_total;
		double countPercent = 100.0*tmpStaticInfo.count / g_count;
		oFile << "total: " << tmpStaticInfo.total << " " << totalPercent << " " 
			<< "count: " << tmpStaticInfo.count << " " << countPercent << std::endl;
		int idx = 0;
		for (int j = 0; j < tmpStaticInfo.vecBt.size(); ++j)
		{
			std::string& tmpStr = tmpStaticInfo.vecBt[j];
			int iPos = tmpStr.find(":");
			std::string strSoPath = tmpStr.substr(0, iPos);
/* 			if (strSoPath.find(SO_NAME) != std::string::npos)
			{
				continue;
			} */
			if (strSoPath == "unknow")
			{
				oFile << "#" << idx++ << " " << strSoPath << std::endl;
				continue;
			}
			std::string strAddr = tmpStr.substr(iPos + 1);
			oFile << "#" << idx++ << " " << strSoPath << " " << strAddr << " " << BtAddr2Line(strSoPath, strAddr).c_str();
		}
		oFile << std::endl;
	}
	printf("time:%ld, completed\n",GetTimeTick());
}

bool JudgeFile(const char* file1,const char* file2)
{
	std::string str1 = file1;
	std::string str2 = file2;
	if(str1 > str2)
	{
		return false;
	}
	std::string sub1 = str1.substr(str1.find("pid"), str1.find("proc") - str1.find("pid"));
	std::string sub2 = str2.substr(str2.find("pid"), str2.find("proc") - str2.find("pid"));
	return sub1 == sub2;
}
int main(int argc, char** argv)
{
	if (argc != 4)
	{
		printf("please input like this : ./btrans xxx.bt yyy.bt xxx.txt\n");
		return -1;
	}
	char* btFile1 = argv[1];
	char* btFile2 = argv[2];
	char* txtFile = argv[3];
	if (!JudgeFile(btFile1, btFile2))
	{
		printf("file1:%s, file2:%s\n", btFile1, btFile2);
		printf("file1 should be the same pid with filw2\n");
		printf("file1 generation time should be less than file2\n");
		return -1;
	}
	BtDiff(btFile1, btFile2, txtFile);
	return 0;
}