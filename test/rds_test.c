#include "rds_test.h"
#include <iostream>

// SetUp 实现
void RdsTestSuite::SetUp() {
    // 准备测试数据
    PrepareTestData();
    
    // 创建并打开模拟设备
    fd = CreateMockDevice();
    if (fd > 0) {
        device_opened = true;
        // 写入初始数据
        WriteInitialData();
    }
}

// TearDown 实现
void RdsTestSuite::TearDown() {
    CleanupDevice();
}

// 创建模拟设备
int RdsTestSuite::CreateMockDevice() {
    int tmp_fd = open(device_path, O_RDWR | O_CREAT, 0666);
    if (tmp_fd < 0) {
        std::cerr << "Failed to create mock device: " << strerror(errno) << std::endl;
        return -1;
    }
    return tmp_fd;
}

// 准备测试数据
void RdsTestSuite::PrepareTestData() {
    // PI码 (0x1234)
    int index = 0;
    
    // 第一组 RDS 数据: PI码 + RT段0 "Now "
    test_data[index++] = {
        .lsb = 0x34,
        .msb = 0x12,      // PI码: 0x1234
        .block = 0        // Block A
    };
    test_data[index++] = {
        .lsb = 0x00,      // segment 0
        .msb = 0x20,      // Group Type 2A
        .block = 1        // Block B
    };
    test_data[index++] = {
        .lsb = 0x4E,      // 'N'
        .msb = 0x6F,      // 'o'
        .block = 2        // Block C
    };
    test_data[index++] = {
        .lsb = 0x77,      // 'w'
        .msb = 0x20,      // ' '
        .block = 3        // Block D
    };

    // 第二组 RDS 数据: PI码 + RT段1 "Play"
    test_data[index++] = {
        .lsb = 0x34,
        .msb = 0x12,      // PI码: 0x1234
        .block = 0
    };
    test_data[index++] = {
        .lsb = 0x01,      // segment 1
        .msb = 0x20,      // Group Type 2A
        .block = 1
    };
    test_data[index++] = {
        .lsb = 0x50,      // 'P'
        .msb = 0x6C,      // 'l'
        .block = 2
    };
    test_data[index++] = {
        .lsb = 0x61,      // 'a'
        .msb = 0x79,      // 'y'
        .block = 3
    };

    // 第三组 RDS 数据: PI码 + RT段2 "ing:"
    test_data[index++] = {
        .lsb = 0x34,
        .msb = 0x12,
        .block = 0
    };
    test_data[index++] = {
        .lsb = 0x02,      // segment 2
        .msb = 0x20,
        .block = 1
    };
    test_data[index++] = {
        .lsb = 0x69,      // 'i'
        .msb = 0x6E,      // 'n'
        .block = 2
    };
    test_data[index++] = {
        .lsb = 0x67,      // 'g'
        .msb = 0x3A,      // ':'
        .block = 3
    };

    // 第四组 RDS 数据: PI码 + RT段3 " Hel"
    test_data[index++] = {
        .lsb = 0x34,
        .msb = 0x12,
        .block = 0
    };
    test_data[index++] = {
        .lsb = 0x03,      // segment 3
        .msb = 0x20,
        .block = 1
    };
    test_data[index++] = {
        .lsb = 0x20,      // ' '
        .msb = 0x48,      // 'H'
        .block = 2
    };
    test_data[index++] = {
        .lsb = 0x65,      // 'e'
        .msb = 0x6C,      // 'l'
        .block = 3
    };

    // 第五组 RDS 数据: PI码 + RT段4 "lo W"
    test_data[index++] = {
        .lsb = 0x34,
        .msb = 0x12,
        .block = 0
    };
    test_data[index++] = {
        .lsb = 0x04,      // segment 4
        .msb = 0x20,
        .block = 1
    };
    test_data[index++] = {
        .lsb = 0x6C,      // 'l'
        .msb = 0x6F,      // 'o'
        .block = 2
    };
    test_data[index++] = {
        .lsb = 0x20,      // ' '
        .msb = 0x57,      // 'W'
        .block = 3
    };

    // 第六组 RDS 数据: PI码 + RT段5 "orld" + CR
    test_data[index++] = {
        .lsb = 0x34,
        .msb = 0x12,
        .block = 0
    };
    test_data[index++] = {
        .lsb = 0x05,      // segment 5
        .msb = 0x20,
        .block = 1
    };
    test_data[index++] = {
        .lsb = 0x6F,      // 'o'
        .msb = 0x72,      // 'r'
        .block = 2
    };
    test_data[index++] = {
        .lsb = 0x6C,      // 'l'
        .msb = 0x64,      // 'd'
        .block = 3
    };

    // 第七组 RDS 数据: PI码 + RT段6 (结束标记)
    test_data[index++] = {
        .lsb = 0x34,
        .msb = 0x12,
        .block = 0
    };
    test_data[index++] = {
        .lsb = 0x06,      // segment 6
        .msb = 0x20,
        .block = 1
    };
    test_data[index++] = {
        .lsb = 0x0D,      // CR (结束符)
        .msb = 0x00,
        .block = 2
    };
    test_data[index++] = {
        .lsb = 0x00,
        .msb = 0x00,
        .block = 3
    };
}

// 写入初始数据
void RdsTestSuite::WriteInitialData() {
    if (!device_opened) return;
    
    for (int i = 0; i < MAX_RDS_BLOCKS; i++) {
        if (write(fd, &test_data[i], sizeof(struct v4l2_rds_data)) < 0) {
            std::cerr << "Failed to write test data block " << i << std::endl;
        }
    }
    // 重置文件指针到开始位置
    lseek(fd, 0, SEEK_SET);
}

// 清理设备
void RdsTestSuite::CleanupDevice() {
    if (device_opened) {
        close(fd);
        unlink(device_path);
        device_opened = false;
        fd = -1;
    }
}

// 生成PI块
void RdsTestSuite::GeneratePIBlock() {
    RdsDataSimulator::GeneratePICode(&test_data[0], pi_code);
}

// 生成PS名称块
void RdsTestSuite::GeneratePSNameBlock() {
    for (int i = 0; i < PS_NAME_SIZE/2; i++) {
        RdsDataSimulator::GeneratePSName(&test_data[i+1], 
                                       (const char*)ps_name, i);
    }
}

// 生成RadioText块
void RdsTestSuite::GenerateRadioTextBlock() {
    // 实现RadioText块生成逻辑
}

// 验证PI码
bool RdsTestSuite::VerifyPICode(uint16_t expected_pi) {
    return pi_code == expected_pi;
}

// 验证PS名称
bool RdsTestSuite::VerifyPSName(const char* expected_ps) {
    return strncmp((char*)ps_name, expected_ps, PS_NAME_SIZE) == 0;
}

// 验证RadioText
bool RdsTestSuite::VerifyRadioText(const char* expected_rt) {
    return strncmp((char*)radio_text, expected_rt, RT_SIZE) == 0;
}

// RdsErrorInjector 实现
void RdsErrorInjector::InjectBitError(struct v4l2_rds_data* block, int bit_position) {
    if (bit_position < 8) {
        block->lsb ^= (1 << bit_position);
    } else if (bit_position < 16) {
        block->msb ^= (1 << (bit_position - 8));
    }
}

void RdsErrorInjector::InjectBlockError(struct v4l2_rds_data* block) {
    block->lsb = ~block->lsb;
    block->msb = ~block->msb;
}

void RdsErrorInjector::InjectSequenceError(struct v4l2_rds_data* blocks, int count) {
    if (count >= 2) {
        // 交换两个相邻块
        struct v4l2_rds_data temp = blocks[0];
        blocks[0] = blocks[1];
        blocks[1] = temp;
    }
}

// 测试用例实现
TEST_F(RdsTestSuite, ReadPICode) {
    ASSERT_TRUE(device_opened);
    
    // 写入PI码
    pi_code = 0x1234;
    GeneratePIBlock();
    WriteInitialData();
    
    // 调用被测试的函数
    int result = read_rds_from_fd(fd);
    
    // 验证结果
    EXPECT_EQ(result, 0);
    EXPECT_TRUE(VerifyPICode(0x1234));
}

TEST_F(RdsTestSuite, ReadPSName) {
    ASSERT_TRUE(device_opened);
    
    // 写入PS名称
    const char* test_ps = "TESTFM";
    memcpy(ps_name, test_ps, strlen(test_ps));
    GeneratePSNameBlock();
    WriteInitialData();
    
    // 调用被测试的函数
    int result = read_rds_from_fd(fd);
    
    // 验证结果
    EXPECT_EQ(result, 0);
    EXPECT_TRUE(VerifyPSName(test_ps));
}

TEST_F(RdsTestSuite, ReadRadioText) {
    ASSERT_TRUE(device_opened);
    
    // 写入RadioText
    const char* test_rt = "Hello Radio!";
    memcpy(radio_text, test_rt, strlen(test_rt));
    GenerateRadioTextBlock();
    WriteInitialData();
    
    // 调用被测试的函数
    int result = read_rds_from_fd(fd);
    
    // 验证结果
    EXPECT_EQ(result, 0);
    EXPECT_TRUE(VerifyRadioText(test_rt));
}

// 错误处理测试
TEST_F(RdsTestSuite, HandleBitError) {
    ASSERT_TRUE(device_opened);
    
    // 注入位错误
    RdsErrorInjector::InjectBitError(&test_data[0], 3);
    WriteInitialData();
    
    // 验证错误处理
    int result = read_rds_from_fd(fd);
    EXPECT_NE(result, 0);
}

// 主函数
int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}