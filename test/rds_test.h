#ifndef RDS_TEST_H
#define RDS_TEST_H

#include <gtest/gtest.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

// RDS 数据结构定义
struct v4l2_rds_data {
    uint8_t lsb;     // 数据的最低有效字节
    uint8_t msb;     // 数据的最高有效字节
    uint8_t block;   // 块标识符 (0-3)
};

// RDS 块类型定义
enum rds_block_type {
    RDS_BLOCK_A = 0,  // PI 码
    RDS_BLOCK_B = 1,  // 组类型码和其他控制信息
    RDS_BLOCK_C = 2,  // 依赖于组类型
    RDS_BLOCK_D = 3   // 依赖于组类型
};

// 测试套件类
class RdsTestSuite : public ::testing::Test {
protected:
    // 测试相关的常量
    static const int MAX_RDS_BLOCKS = 10;
    static const int PS_NAME_SIZE = 8;
    static const int RT_SIZE = 64;

    // 测试设备路径
    const char* device_path;
    
    // 文件描述符
    int fd;
    
    // 测试数据
    struct v4l2_rds_data test_data[MAX_RDS_BLOCKS];
    
    // RDS 数据缓冲区
    uint8_t ps_name[PS_NAME_SIZE];  // Program Service Name
    uint8_t radio_text[RT_SIZE];    // Radio Text
    uint16_t pi_code;               // Program Identification
    
    // 测试状态标志
    bool device_opened;
    uint32_t valid_fields;

    // 测试夹具设置和清理
    void SetUp() override;
    void TearDown() override;

    // 辅助方法
    int CreateMockDevice();
    void PrepareTestData();
    void WriteInitialData();
    void CleanupDevice();

    // RDS 数据生成方法
    void GeneratePIBlock();
    void GeneratePSNameBlock();
    void GenerateRadioTextBlock();
    
    // 验证方法
    bool VerifyPICode(uint16_t expected_pi);
    bool VerifyPSName(const char* expected_ps);
    bool VerifyRadioText(const char* expected_rt);

public:
    RdsTestSuite() : 
        device_path("/tmp/mock_radio"),
        fd(-1),
        device_opened(false),
        valid_fields(0)
    {
        memset(ps_name, 0, PS_NAME_SIZE);
        memset(radio_text, 0, RT_SIZE);
        pi_code = 0;
    }

    // 用于外部访问的方法
    int GetFileDescriptor() const { return fd; }
    bool IsDeviceOpen() const { return device_opened; }
};

// RDS 数据模拟器类
class RdsDataSimulator {
public:
    static void GenerateBlock(struct v4l2_rds_data* block, 
                            uint8_t lsb, 
                            uint8_t msb, 
                            uint8_t block_type) {
        block->lsb = lsb;
        block->msb = msb;
        block->block = block_type;
    }

    static void GeneratePICode(struct v4l2_rds_data* block, uint16_t pi) {
        GenerateBlock(block, 
                     pi & 0xFF,        // LSB
                     (pi >> 8) & 0xFF, // MSB
                     RDS_BLOCK_A);
    }

    static void GeneratePSName(struct v4l2_rds_data* block, 
                             const char* ps_name,
                             uint8_t position) {
        uint16_t chars = (ps_name[position * 2] << 8) | ps_name[position * 2 + 1];
        GenerateBlock(block,
                     chars & 0xFF,
                     (chars >> 8) & 0xFF,
                     RDS_BLOCK_D);
    }
};

// 错误注入接口
class RdsErrorInjector {
public:
    static void InjectBitError(struct v4l2_rds_data* block, int bit_position);
    static void InjectBlockError(struct v4l2_rds_data* block);
    static void InjectSequenceError(struct v4l2_rds_data* blocks, int count);
};

#endif // RDS_TEST_H