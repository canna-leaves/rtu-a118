/*
 * map.c
 *
 *  Created on: Dec 17, 2014
 *      Author: ygz
 */

#include <unistd.h>
#include <libio.h>
#include <stdio.h>
#include <stdlib.h>
#include <regex.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <error.h>
#include <errno.h>
#include <time.h>
#include "config.h"
#include "common/common.h"
#include "map.h"
#include "gpio/leds.h"
#include "modbus-lock.h"


#define PATH_MAP0         "/usr/share/map0.csv"         //总映射表
#define PATH_MAP1         "/usr/share/map1.csv"         //flash1映射表
#define PATH_FLASH1       "/usr/share/flash1"           //flash1存储位置
#define PATH_FLASH2       "/usr/share/flash2"           //flash2存储位置
#define PATH_MAP_001      "/usr/share/map_001.csv"      //默认映射表(油井)

#define FLASH1_MAX_LEN    RTU_FLASH1_REGISTER_SIZE
#define MAX_WELL_NUM      8                             //最大井口数
#define MAX_REGISTER_NUM  RTU_MAX_READ_REGISTER

#define VOLTAGE_RATIO             22.277                //电阻比值
#define VOLTAGE_RATIO_DISPLAY     100                   //电压寄存器显示比例
#define MAX_VOLTAGE               1.8                   //am335x电压基准值上限
#define DI_SWITCH_ON              1

enum {
	MAP_NULL,       //空映射
	MAP_RAM,        //ram映射
	MAP_FLASH1,     //flash1映射
	MAP_FLASH2,     //flash2映射
	MAP_SYS,        //函数映射
	MAP_RECORD,     //记录类型
	MAP_RO,         //只读类型
	MAP_RW,         //读写类型
};

//#define printf(format, arg...) fprintf(stdout, "%s %d: "format, __FUNCTION__, __LINE__, ##arg)
#if 0
#define perror(format, arg...) \
		fprintf(stderr, "%s %d: "format" ERROR %d %s\n", __FUNCTION__, __LINE__, ##arg, errno, strerror(errno))
#endif
Map_t map_map_t;    //映射表

/*
 * 获取文件大小
 * path in 文件路径
 */
unsigned long get_file_size(const char *path)
{
	unsigned long filesize = -1;
	struct stat statbuff;
	if(stat(path, &statbuff) < 0){
		return filesize;
	}else{
		filesize = statbuff.st_size;
	}
	return filesize;
}

/* 获取单个ram映射区大小 */
int get_x_y_z_max_len(uint16_t *x_max) {
	FILE *file_map0 = fopen(PATH_MAP0, "r");
	if(NULL == file_map0) {
		perror("%s open error", PATH_MAP0);
		return -1;
	}

	char buf_map0[512];
	char buf_map_00i_path_path[512];
	char buf_map_00i[512];
	int rc = 0;
	uint16_t well_type;
	FILE *file_map_00i = NULL;
	while(EOF != fscanf(file_map0, "%s\r", buf_map0)) {
		rc = sscanf(buf_map0, "%hd,%s", &well_type, buf_map_00i_path_path);
		if(2 != rc) {
			printf("rc=%d, well_type=%hd, buf_map0_path=%s\n", rc, well_type, buf_map_00i_path_path);
			return -1;
		}
		printf("rc=%d, well_type=%hd, buf_map0_path=%s\n", rc, well_type, buf_map_00i_path_path);
		file_map_00i = fopen(buf_map_00i_path_path, "r");
		if(NULL == file_map_00i) {
			perror("%s open error", buf_map_00i_path_path);
			return -1;
		}
		uint16_t x0, x1, y0, y1;
		while(EOF != fscanf(file_map_00i, "%s\r", buf_map_00i)) {
			if(NULL == strstr(buf_map_00i, "~")) {
				rc = sscanf(buf_map_00i, "%hd,%hd", &x0, &y0);
				if(2 != rc) {
					perror("rc=%d, buf_map_00i=%s\n", rc, buf_map_00i);
					return -1;
				}
				if(*x_max < x0) {
					*x_max = x0;
				}
//				printf("%s\n", buf_map_00i);
			} else {
				rc = sscanf(buf_map_00i, "%hd~%hd,%hd~%hd", &x0, &x1, &y0, &y1);
				if(4 != rc) {
					perror("rc=%d, buf_map_00i=%s\n", rc, buf_map_00i);
					return -1;
				}
				if(*x_max < x1) {
					*x_max = x1;
				}
//				printf("~%s\n", buf_map_00i);
			}
		}
		fclose(file_map_00i);
	}
	fclose(file_map0);
	*x_max += 1;
	if(*x_max > MAX_REGISTER_NUM) {
		perror("*x_max too max, *x_max=%d", *x_max);
		return -1;
	}
	return 0;
}

/*
 * 创建flash
 * path in 路径
 * len in 长度
 * flash_is_new out flash首次初始化标识
 */
int flash_create(const char *path, int len, char *flash_is_new) {
	void *flash_init_buf;
	FILE *flash_file;
	if(0 != access(path, 0)
			|| (len != get_file_size(path))) {
		*flash_is_new = 1;
		flash_init_buf = malloc(len);
		if(NULL == flash_init_buf) {
			perror("flash_init_buf=NULL");
			return -1;
		}
		memset(flash_init_buf, -1, len);
		flash_file = fopen(path, "w");
		if(NULL == flash_file) {
			perror("flash_file=NULL");
			return -1;
		}
		if(1 != fwrite(flash_init_buf, len, 1, flash_file)) {
			perror("flash fwrite error");
			return -1;
		}
		fclose(flash_file);
		free(flash_init_buf);
		printf("create flash %s\n", path);
	}
	return 0;
}

/*
 * flash 内存映射
 */
int flash_map(const char *path, int len, void **flash) {
	int fd = open(path, O_RDWR | O_SYNC);
	if(-1 == fd) {
		perror("flash_map fd=-1");
		return -1;
	}
	*flash = mmap(NULL, len ,PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if(MAP_FAILED == *flash) {
		perror("flash map error");
		return -1;
	}
	close(fd);
	return 0;
};

/* ram flash 创建 */
int ram_flash_create(void **ram,
		uint16_t **ram_x,
		char **ram_x_map_type,
		char **ram_x_map_record,
		char **ram_x_map_rw,
		uint16_t **ram_y,
		char **ram_y_map_type,
		uint16_t **flash1,
		uint16_t **flash2,
		uint16_t x_max,
		char *flash1_is_new,
		char *flash2_is_new) {
	int ram_len = x_max * MAX_WELL_NUM * sizeof(uint16_t)
			+ x_max * MAX_WELL_NUM * sizeof(char)
			+ x_max * MAX_WELL_NUM * sizeof(char)
			+ x_max * MAX_WELL_NUM * sizeof(char)
			+ MAX_REGISTER_NUM * MAX_WELL_NUM * sizeof(uint16_t)
			+ MAX_REGISTER_NUM * MAX_WELL_NUM * sizeof(char);
	printf("ram_len=%d\n", ram_len);
	*ram = malloc(ram_len);
	if(NULL == *ram) {
		perror("ram=NULL");
		return -1;
	}
	*ram_x = (uint16_t *)(*ram);
	memset(*ram_x, 0xFF, x_max * MAX_WELL_NUM * sizeof(uint16_t));
	*ram_x_map_type = (char *)(*ram_x + x_max * MAX_WELL_NUM);
	memset(*ram_x_map_type, MAP_NULL, x_max * MAX_WELL_NUM * sizeof(char));
	*ram_x_map_record = (char *)(*ram_x_map_type + x_max * MAX_WELL_NUM);
	memset(*ram_x_map_record, MAP_NULL, x_max * MAX_WELL_NUM * sizeof(char));
	*ram_x_map_rw = (*ram_x_map_record + x_max * MAX_WELL_NUM);
	memset(*ram_x_map_rw, MAP_RO, x_max * MAX_WELL_NUM * sizeof(char));

	*ram_y = (uint16_t *)(*ram_x_map_rw + x_max * MAX_WELL_NUM);
	memset(*ram_y, 0xFF, MAX_REGISTER_NUM * MAX_WELL_NUM * sizeof(uint16_t));
	*ram_y_map_type = (char *)(*ram_y + MAX_REGISTER_NUM * MAX_WELL_NUM);
	memset(*ram_y_map_type, MAP_NULL, MAX_REGISTER_NUM * MAX_WELL_NUM * sizeof(char));
	printf("ram create ok\n");
	printf("flash1_len %u, flash2_len %u\n", FLASH1_MAX_LEN * sizeof(uint16_t),
			x_max * MAX_WELL_NUM * sizeof(uint16_t));
	if(-1 == flash_create(PATH_FLASH1, FLASH1_MAX_LEN * sizeof(uint16_t), flash1_is_new)) {
		perror("flash 1 create error %s\n", PATH_FLASH1);
		return -1;
	}
	if(-1 == flash_create(PATH_FLASH2, x_max * MAX_WELL_NUM * sizeof(uint16_t), flash2_is_new)) {
		perror("flash 2 create error %s\n", PATH_FLASH2);
		return -1;
	}

	if(-1 == flash_map(PATH_FLASH1, FLASH1_MAX_LEN * sizeof(uint16_t), (void **)flash1)) {
		perror("flash 1 map error %s\n", PATH_FLASH1);
		return -1;
	}

	if(-1 == flash_map(PATH_FLASH2, x_max * MAX_WELL_NUM * sizeof(uint16_t), (void **)flash2)) {
		perror("flash 2 map error %s\n", PATH_FLASH2);
		return -1;
	}
	printf("flash create ok\n");

	return 0;
}

/* ram flash 初始化 */
int ram_map_flash1_value_init(uint16_t *ram_x,
		char *ram_x_map_type,
		char *ram_x_map_record,
		char *ram_x_map_rw,
		uint16_t *ram_y,
		char *ram_y_map_type,
		uint16_t *flash1,
		uint16_t *flash2,
		char flash1_is_new,
		char flash2_is_new,
		uint16_t x_max,
		const char *path,
		char is_flash1) {
	FILE *file = fopen(path, "r");
	if(NULL == file) {
		return -1;
	}
	char buf_map1[512];
	uint16_t x0, x1, y0, y1, d_v;
	int rc;
	while(EOF != fscanf(file, "%s\r", buf_map1)) {
		char map_type = MAP_NULL;
		char map_record = MAP_NULL;
		char map_rw = MAP_RO;
		if(NULL == strstr(buf_map1, "~")) {
			if(NULL != strstr(buf_map1, "0x")) {
				rc = sscanf(buf_map1, "%hd,%hd,%hx", &x0, &y0, &d_v);
			} else {
				rc = sscanf(buf_map1, "%hd,%hd,%hd", &x0, &y0, &d_v);
			}
			if(3 != rc) {
				perror("rc=%d, buf_map1=%s\n", rc, buf_map1);
				return -1;
			}
			if(y0 - 40001 < 0) {
				y0 = 0;
			} else {
				y0 -= 40001;
			}

//			printf("%s\n", buf_map1);
			if(NULL != strstr(buf_map1, "FLASH1")) {
				map_type = MAP_FLASH1;
//				printf("----------------%s\n", buf_map1);
			} else if(NULL != strstr(buf_map1, "FLASH2")) {
				map_type = MAP_FLASH2;
//				printf("----------------%s\n", buf_map1);
			} else if(NULL != strstr(buf_map1, "SYS")) {
				map_type = MAP_SYS;
			}

			if(NULL != strstr(buf_map1, "RECORD")) {
				map_record = MAP_RECORD;
			}

			if(NULL != strstr(buf_map1, "WR")) {
				map_rw = MAP_RW;
			}

			if(NULL != strstr(buf_map1, "RW")) {
				map_rw = MAP_RW;
			}

			*(ram_x + x0) = d_v;
			*(ram_x_map_type + x0) = map_type;
			*(ram_x_map_record + x0) = map_record;
			*(ram_x_map_rw + x0) = map_rw;
			*(ram_y + y0) = x0;
			*(ram_y_map_type + y0) = MAP_RAM;
			if((MAP_SYS == map_type || MAP_FLASH1 == map_type) && flash1_is_new) {
				*(flash1 + x0) = d_v;
			} else if((MAP_SYS == map_type || MAP_FLASH2 == map_type) && flash2_is_new) {
				*(flash2 + x0) = d_v;
//				printf("MAP_FLASH2 %hd %hd, %hd, ", x0, d_v, *(flash2 + x0));
//				printf("flash2   flash2 %hd\n", *flash2);
			}
//			printf("%hd, %p, ", *(ram_x + x0), ram_x);
//			printf("%s d_v %d y0 %d\n", buf_map1, d_v, y0);
		} else {
			if(NULL != strstr(buf_map1, "0x")) {
				rc = sscanf(buf_map1, "%hd~%hd,%hd~%hd,%hx", &x0, &x1, &y0, &y1, &d_v);
			} else {
				rc = sscanf(buf_map1, "%hd~%hd,%hd~%hd,%hd", &x0, &x1, &y0, &y1, &d_v);
			}
			if(5 != rc) {
				perror("rc=%d, buf_map1=%s\n", rc, buf_map1);
				return -1;
			}
			if(y0 - 40001 < 0) {
				y0 = 0;
			} else {
				y0 -= 40001;
			}
			if(y1 - 40001 < 0) {
				y1 = 0;
			} else {
				y1 -= 40001;
			}
			if(y1 - y0 <= 0 || x1 - x0 <= 0) {
				printf("y1 - y0 <= 0 || x1 - x0 <= 0 %s\n", buf_map1);
				continue;
			}

			if(NULL != strstr(buf_map1, "FLASH1")) {
				map_type = MAP_FLASH1;
			} else if(NULL != strstr(buf_map1, "FLASH2")) {
				map_type = MAP_FLASH2;
			} else if(NULL != strstr(buf_map1, "SYS")) {
				map_type = MAP_SYS;
			}

			if(NULL != strstr(buf_map1, "RECORD")) {
				map_record = MAP_RECORD;
			}

			if(NULL != strstr(buf_map1, "WR")) {
				map_rw = MAP_RW;
			}

			if(NULL != strstr(buf_map1, "RW")) {
				map_rw = MAP_RW;
			}

			uint16_t i = x0, j = y0;
			for(;i <= x1;++i, ++j) {
				*(ram_x + i) = d_v;
				*(ram_x_map_type + i) = map_type;
				*(ram_x_map_record + i) = map_record;
				*(ram_x_map_rw + i) = map_rw;
				*(ram_y + j) = i;
				*(ram_y_map_type + j) = MAP_RAM;
				if((MAP_SYS == map_type || MAP_FLASH1 == map_type) && flash1_is_new) {
					*(flash1 + i) = d_v;
				} else if((MAP_SYS == map_type || MAP_FLASH2 == map_type) && flash2_is_new) {
					*(flash2 + i) = d_v;
				}
//				printf("%s d_v %d j %d\n", buf_map1, d_v, j);
			}
		}
	}
	fclose(file);
//	*flash2 = 1;
	if(!is_flash1) {
		printf("ram_map_flash2_value_init ok\n");
		return 0;
	}
	int well_index;
	if(flash2_is_new) {
//		memcpy(flash2, flash1, FLASH1_MAX_LEN * sizeof(uint16_t));
		printf("cp flash1 to flash2\n");
	}
	for(well_index = 1;well_index < MAX_WELL_NUM;++well_index) {
		memcpy(ram_x + well_index * x_max, ram_x, FLASH1_MAX_LEN * sizeof(uint16_t));
		memcpy(ram_x_map_type + well_index * x_max, ram_x_map_type, FLASH1_MAX_LEN * sizeof(char));
		memcpy(ram_x_map_record + well_index * x_max, ram_x_map_record, FLASH1_MAX_LEN * sizeof(char));
		memcpy(ram_x_map_rw + well_index * x_max, ram_x_map_rw, FLASH1_MAX_LEN * sizeof(char));
		memcpy(ram_y + well_index * MAX_REGISTER_NUM, ram_y, MAX_REGISTER_NUM * sizeof(uint16_t));
		memcpy(ram_y_map_type + well_index * MAX_REGISTER_NUM, ram_y_map_type, MAX_REGISTER_NUM * sizeof(char));
		if(flash2_is_new) {
			memcpy(flash2 + well_index * x_max, flash2, FLASH1_MAX_LEN * sizeof(uint16_t));
		}
	}
	printf("ram_map_flash1_value_init ok\n");
	return 0;
}

/* 获取映射表路径 */
int get_map_00_path(uint16_t type, char *path) {
	FILE *file_map0 = fopen(PATH_MAP0, "r");
	if(NULL == file_map0) {
		perror("file_map0=NULL");
		return -1;
	}
	char buf_map0[512];
	char buf_map_00i_path_path[512];
	uint16_t well_type;
	int rc = -1;
	while(EOF != fscanf(file_map0, "%s\r", buf_map0)) {
		rc = sscanf(buf_map0, "%hd,%s", &well_type, buf_map_00i_path_path);
		//printf("rc=%d, well_type=%hd, buf_map0_path=%s\n", rc, well_type, buf_map_00i_path_path);
		if(2 != rc) {
			printf("rc=%d, well_type=%hd, buf_map0_path=%s\n", rc, well_type, buf_map_00i_path_path);
			rc = -1;
			break;
			//return -1;
		}
		if(type == well_type) {
			strcpy(path, buf_map_00i_path_path);
			rc = 0;
			break;
			//return type;
		}
		rc = -1;
//		printf("rc=%d, well_type=%hd, buf_map0_path=%s\n", rc, well_type, buf_map_00i_path_path);
	}
	fclose(file_map0);
	return rc;
}

/* flash2 初始化 */
int ram_map_flash2_value_init(uint16_t *ram_x,
		char *ram_x_map_type,
		char *ram_x_map_record,
		char *ram_x_map_rw,
		uint16_t *ram_y,
		char *ram_y_map_type,
		uint16_t *flash1,
		uint16_t *flash2,
		char flash1_is_new,
		char flash2_is_new,
		uint16_t x_max) {
	int well_index;
	uint16_t well_type;
	char map_00i_path[512] = PATH_MAP_001;
	for(well_index = 0;well_index < MAX_WELL_NUM;++well_index) {
		well_type = *(flash2);
		printf("well_type %d\n", well_type);
		if(-1 == get_map_00_path(well_type, map_00i_path)) {
			*flash2 = 1;
			strcpy(map_00i_path, PATH_MAP_001);
			printf("cannot find well type\n");
		}
		printf("%s\n", map_00i_path);
		if(-1 == ram_map_flash1_value_init(ram_x, ram_x_map_type, ram_x_map_record,
				ram_x_map_rw, ram_y, ram_y_map_type, flash1,flash2, 0, flash2_is_new, x_max, map_00i_path, 0)) {
			perror("ram_map_flash1_value_item_init error");
			return -1;
		}
		ram_x += x_max;
		ram_x_map_type += x_max;
		ram_x_map_record += x_max;
		ram_x_map_rw += x_max;
		ram_y += MAX_REGISTER_NUM;
		ram_y_map_type += MAX_REGISTER_NUM;
		flash2 += x_max;
	}
	return 0;
}

/* 重新映射单个映射表 */
int map_item_remap(Map_t *map_t, uint16_t well_num, uint16_t well_type) {
	int well_index = well_num - 1;
	if(well_index < 0 || well_index > MAX_WELL_NUM) {
		return -1;
	}
	char map_00i_path[512] = PATH_MAP_001;
	if(-1 == get_map_00_path(well_type, map_00i_path)) {
		printf("cannot get path of type %hd\n", well_type);
		return -1;
	}
	printf("remap %s well_type %d\n", map_00i_path, well_type);

	uint16_t x_max = map_t->x_max;
	uint16_t *ram_x = map_t->ram_x + well_index * x_max;
	char *ram_x_map_type = map_t->ram_x_map_type + well_index * x_max;
	char *ram_x_map_record = map_t->ram_x_map_record + well_index * x_max;
	char *ram_x_map_rw = map_t->ram_x_map_rw + well_index * x_max;
	uint16_t *ram_y = map_t->ram_y + well_index * MAX_REGISTER_NUM;
	char *ram_y_map_type = map_t->ram_y_map_type + well_index * MAX_REGISTER_NUM;
	uint16_t *flash1 = map_t->flash1;
	uint16_t *flash2 = map_t->flash2 + well_index * x_max;
	//char flash1_is_new = 0;
	char flash2_is_new = 1;
	*flash2 = well_type;
	if(-1 == ram_map_flash1_value_init(ram_x, ram_x_map_type, ram_x_map_record,
			ram_x_map_rw, ram_y, ram_y_map_type, flash1,flash2, 0, flash2_is_new, x_max, map_00i_path, 0)) {
		perror("ram_map_flash1_value_item_init error");
		return -1;
	}
	if(-1 == msync(map_t->flash2, map_t->x_max * MAX_WELL_NUM * sizeof(uint16_t), MS_SYNC)) {
		perror("flash2 msync error");
		return -1;
	}
	return 0;
}

int reset_flash1_to_default() {
	Map_t *map_t = &map_map_t;
	uint16_t x_max = map_t->x_max;
	uint16_t *ram_x = map_t->ram_x;
	char *ram_x_map_type = map_t->ram_x_map_type;
	char *ram_x_map_record = map_t->ram_x_map_record;
	char *ram_x_map_rw = map_t->ram_x_map_rw;
	uint16_t *ram_y = map_t->ram_y;
	char *ram_y_map_type = map_t->ram_y_map_type;
	uint16_t *flash1 = map_t->flash1;
	uint16_t *flash2 = map_t->flash2;
	char flash1_is_new = 1;
	char flash2_is_new = 0;
	
	if(-1 == ram_map_flash1_value_init(ram_x, ram_x_map_type, ram_x_map_record, ram_x_map_rw,
			ram_y, ram_y_map_type, flash1, flash2, flash1_is_new, flash2_is_new, x_max,
			PATH_MAP1, 0)) {
		perror("ram_map_flash1_value_init error");
		return -1;
	}
	uint16_t v = 1;
	rtu_reset_flash1(MAP_CONFIG1, &v);

	if(-1 == msync(flash1, FLASH1_MAX_LEN * sizeof(uint16_t), MS_SYNC)) {
		perror("flash1 msync error");
		return -1;
	}
	return 0;
}

/* 映射初始化 */
int map_init(Map_t *map_t) {
	uint16_t x_max = 0;
	if(-1 == get_x_y_z_max_len(&x_max)) {
		perror("get_x_y_z_max_len error");
		return -1;
	}
	printf("x_max=%d\n", x_max);

	void *ram;
	uint16_t *ram_x;
	char *ram_x_map_type;
	char *ram_x_map_record;
	char *ram_x_map_rw;
	uint16_t *ram_y;
	char *ram_y_map_type;
	uint16_t *flash1;
	uint16_t *flash2;
	char flash1_is_new = 0;
	char flash2_is_new = 0;
	//int rc;

	if(-1 == ram_flash_create(&ram, &ram_x, &ram_x_map_type, &ram_x_map_record, &ram_x_map_rw,
			&ram_y, &ram_y_map_type, &flash1, &flash2, x_max, &flash1_is_new, &flash2_is_new)) {
		perror("ram_flash_create error");
		return -1;
	}
//	printf("ram ram ram %p ram_y_map_type %p %d\n", ram, ram_y_map_type, (char *)ram_y_map_type - (char *)ram);
	printf("ram_flash_create ok, flash1_is_new=%d, flash2_is_new=%d\n", flash1_is_new, flash2_is_new);

//	printf("flash2   flash2 %hd\n", *flash2);
	if(-1 == ram_map_flash1_value_init(ram_x, ram_x_map_type, ram_x_map_record, ram_x_map_rw,
			ram_y, ram_y_map_type, flash1, flash2, flash1_is_new, flash2_is_new, x_max,
			PATH_MAP1, 1)) {
		perror("ram_map_flash1_value_init error");
		return -1;
	}
//	printf("flash2   flash2 %hd\n", *flash2);

	if(-1 == ram_map_flash2_value_init(ram_x, ram_x_map_type, ram_x_map_record, ram_x_map_rw,
			ram_y, ram_y_map_type, flash1, flash2, flash1_is_new, flash2_is_new, x_max)) {
		perror("ram_map_flash2_value_init error");
		return -1;
	}

	if(-1 == msync(flash1, FLASH1_MAX_LEN * sizeof(uint16_t), MS_SYNC)) {
		perror("flash1 msync error");
		return -1;
	}

	if(-1 == msync(flash2, x_max * MAX_WELL_NUM * sizeof(uint16_t), MS_SYNC)) {
		perror("flash2 msync error");
		return -1;
	}
	map_t->ram = ram;
	map_t->ram_x = ram_x;
	map_t->ram_x_map_type = ram_x_map_type;
	map_t->ram_x_map_record = ram_x_map_record;
	map_t->ram_x_map_rw = ram_x_map_rw;
	map_t->ram_y = ram_y;
	map_t->ram_y_map_type = ram_y_map_type;
	map_t->flash1 = flash1;
	map_t->flash2 = flash2;
	map_t->flash1_is_new = flash1_is_new;
	map_t->flash2_is_new = flash2_is_new;
	map_t->x_max = x_max;

	map_map_t = *map_t;
	return 0;
}

/* 获取系统时间 */
int map_gettime(uint16_t addr, uint16_t *v) {
	time_t a;
	time(&a);
	struct tm* st_time;
	st_time = localtime(&a);
	//uint16_t rc;
	switch(addr){
	case MAP_GET_TIME_HOUR:
		*v = int16_to_bcd(st_time->tm_hour);
		break;
	case MAP_GET_TIME_MIN:
		*v = int16_to_bcd(st_time->tm_min);
		break;
	case MAP_GET_TIME_SEC:
		*v = int16_to_bcd(st_time->tm_sec);
		break;
	case MAP_GET_TIME_YEAR:
		*v = int16_to_bcd(st_time->tm_year + 1900);
		break;
	case MAP_GET_TIME_MON:
		*v = int16_to_bcd(st_time->tm_mon + 1);
		break;
	case MAP_GET_TIME_DAY:
		*v = int16_to_bcd(st_time->tm_mday);
		break;
	}
	return 0;
}

/* 设置系统时间 */
int map_settime(uint16_t addr, uint16_t *v) {
	time_t a;
	time(&a);
	struct tm* st_time;
	st_time = localtime(&a);
	//uint16_t rc;
	switch(addr){
	case MAP_GET_TIME_HOUR:
		st_time->tm_hour = bcd_to_int16(*v);
		break;
	case MAP_GET_TIME_MIN:
		st_time->tm_min = bcd_to_int16(*v);
		break;
	case MAP_GET_TIME_SEC:
		st_time->tm_sec = bcd_to_int16(*v);
		break;
	case MAP_GET_TIME_YEAR:
		st_time->tm_year = bcd_to_int16(*v) - 1900;
		break;
	case MAP_GET_TIME_MON:
		st_time->tm_mon = bcd_to_int16(*v) - 1;
		break;
	case MAP_GET_TIME_DAY:
		st_time->tm_mday = bcd_to_int16(*v);
		break;
	}
	struct timeval c_time = {mktime(st_time), 0};
	settimeofday(&c_time, NULL);
	rtu_a118_t.time_set_status += 1;
	return 0;
}

/* 30配置 */
int rtu_reset_flash1(uint16_t addr, uint16_t *v) {
	int rc = MSG_CONFIG_START;
	if(1 == *v) {
		pmsg_q_send(rtu_a118_t.zigbee_msg_fd, &rc, sizeof(int), 0);
		printf("after send msg to zigbee_msg_fd %d\n", rtu_a118_t.zigbee_msg_fd);
		pmsg_q_send(rtu_a118_t.tcp_server_msg_fd, &rc, sizeof(int), 0);
		printf("after send msg to tcp_server_msg_fd %d\n", rtu_a118_t.tcp_server_msg_fd);
		pmsg_q_send(rtu_a118_t.udp_server_msg_fd, &rc, sizeof(int), 0);
		printf("after send msg to udp_server_msg_fd %d\n", rtu_a118_t.udp_server_msg_fd);
		pmsg_q_send(rtu_a118_t.rtu_server_msg_fd, &rc, sizeof(int), 0);
		printf("after send msg to rtu_server_msg_fd %d\n", rtu_a118_t.rtu_server_msg_fd);
		pmsg_q_send(rtu_a118_t.tcp_client_msg_fd, &rc, sizeof(int), 0);
		printf("after send msg to tcp_client_msg_fd %d\n", rtu_a118_t.tcp_client_msg_fd);
		pmsg_q_send(rtu_a118_t.rtu_client_msg_fd, &rc, sizeof(int), 0);
		printf("after send msg to rtu_client_msg_fd %d\n", rtu_a118_t.rtu_client_msg_fd);
		*v = 0;
		rtu_a118_t.flash1_reset_status += 1;
	}
	return 0;
}

/* 电参同步 */
int rtu_conf_elect_interval(uint16_t addr, uint16_t *v) {
	int rc = MSG_CONFIG2_START;
	pmsg_q_send(rtu_a118_t.rtu_client_msg_fd, &rc, sizeof(int), 0);
	return 0;
}

/* 执行器同步 */
int rtu_conf_act_interval(uint16_t addr, uint16_t *v) {
	int rc = MSG_CONFIG2_START;
	pmsg_q_send(rtu_a118_t.rtu_client_msg_fd, &rc, sizeof(int), 0);
	return 0;
}

/* 获取历史数据 */
int set_history_data(uint16_t addr, uint16_t *v, uint16_t well_num) {
	int rc[2] = {MSG_REQUEST_H_DATA, well_num};
	pmsg_q_send(rtu_a118_t.data_msg_fd, rc, sizeof(rc), 0);
	return 0;
}

/* 获取电压 */
int get_battery_voltage(uint16_t addr, uint16_t *v, uint16_t well_num) {
	char buf[64];
	int rc;
	if(-1 == rtu_a118_t.adc_fd) {
		return -1;
	}
	lseek(rtu_a118_t.adc_fd, 0, SEEK_SET);
	rc = read(rtu_a118_t.adc_fd, buf, sizeof(buf) - 1);
	if(rc <= 0) {
		perror("battery_voltage read error rc <= 0");
		return -1;
	}
	buf[rc - 1] = '\0';
	rc = sscanf(buf, "%hd", v);
	if(rc != 1) {
		perror("battery_voltage scan error");
		*v = -1;
		return -1;
	}
	float ret = *v * VOLTAGE_RATIO * 
		VOLTAGE_RATIO_DISPLAY * 
		MAX_VOLTAGE / ((2<<11) - 1);
	//printf("voltage %hu %f\n", *v, ret);
	*v = (uint16_t)ret;
	return 0;
}

/* 函数映射读入口 
 * buf out 寄存器输出缓冲区
 * well_num in 井口号
 * addr in 寄存器地址
 * return {-1: 无函数映射, 0: 有函数映射}
 */
int map_map_fun_read(uint16_t *buf, int well_num, int addr) {
	Map_t *map_t = &map_map_t;
	uint16_t *flash2 = map_t->flash2 + (well_num - 1) * map_t->x_max;
	uint16_t *flash2_1 = map_t->flash2 + (1 - 1) * map_t->x_max;
	//uint16_t *ram_x = map_t->ram_x + map_t->x_max * (well_num - 1);
	switch(addr) {
	case MAP_CHANGE_WELL_TYPE:
		*buf = *(flash2 + addr);
		break;
	case MAP_GET_RTU_VER:
		*buf = RTU_VERSION;
		break;
	case MAP_GET_TIME_HOUR:
		map_gettime(addr, buf);
		break;
	case MAP_GET_TIME_MIN:
		map_gettime(addr, buf);
		break;
	case MAP_GET_TIME_SEC:
		map_gettime(addr, buf);
		break;
	case MAP_GET_TIME_YEAR:
		map_gettime(addr, buf);
		break;
	case MAP_GET_TIME_MON:
		map_gettime(addr, buf);
		break;
	case MAP_GET_TIME_DAY:
		map_gettime(addr, buf);
		break;
	case MAP_GET_BATTERY_VOLTAGE:
		get_battery_voltage(addr, buf, well_num);
		break;
	case MAP_CONFIG1:
		*buf = 0;
		break;
	case MAP_GET_MAC0:
		*buf = rtu_a118_t.mac_addr[0];
		break;
	case MAP_GET_MAC1:
		*buf = rtu_a118_t.mac_addr[1];
		break;
	case MAP_GET_MAC2:
		*buf = rtu_a118_t.mac_addr[2];
		break;
	case MAP_GET_MAC3:
		*buf = rtu_a118_t.mac_addr[3];
		break;
	case MAP_GET_MAC4:
		*buf = rtu_a118_t.mac_addr[4];
		break;
	case MAP_GET_MAC5:
		*buf = rtu_a118_t.mac_addr[5];
		break;
	case MAP_CONFIG2_ELEC_INTERV:
		*buf = *(flash2 + addr);
		break;
	case MAP_CONFIG2_ACT_INTERV:
		*buf = *(flash2 + addr);
		break;
	case MAP_GET_DI0:
		gpio_get_do(addr, buf);
		*buf = (*buf & 1)^DI_SWITCH_ON;
		break;
	case MAP_GET_DI1:
		gpio_get_do(addr, buf);
		*buf = (*buf & 1)^DI_SWITCH_ON;
		break;
	case MAP_GET_DI2:
		gpio_get_do(addr, buf);
		*buf = (*buf & 1)^DI_SWITCH_ON;
		break;
	case MAP_GET_DI3:
		gpio_get_do(addr, buf);
		*buf = (*buf & 1)^DI_SWITCH_ON;
		break;
	case MAP_GET_DI4:
		gpio_get_do(addr, buf);
		*buf = (*buf & 1)^DI_SWITCH_ON;
		break;
	case MAP_GET_DI5:
		gpio_get_do(addr, buf);
		*buf = (*buf & 1)^DI_SWITCH_ON;
		break;
	case MAP_GET_DI6:
		gpio_get_do(addr, buf);
		*buf = (*buf & 1)^DI_SWITCH_ON;
		break;
	case MAP_GET_DI7:
		gpio_get_do(addr, buf);
		*buf = (*buf & 1)^DI_SWITCH_ON;
		break;
	case MAP_GET_DO0:
		*buf = *(flash2_1 + addr);
		//gpio_get_do(addr, buf);
		break;
	case MAP_GET_DO1:
		*buf = *(flash2_1 + addr);
		//gpio_get_do(addr, buf);
		break;
	case MAP_GET_DO2:
		*buf = *(flash2_1 + addr);
		//gpio_get_do(addr, buf);
		break;
	case MAP_GET_DO3:
		*buf = *(flash2_1 + addr);
		//gpio_get_do(addr, buf);
		break;
	case MAP_GET_DO4:
		*buf = *(flash2_1 + addr);
		//gpio_get_do(addr, buf);
		break;
	case MAP_GET_DO5:
		*buf = *(flash2_1 + addr);
		//gpio_get_do(addr, buf);
		break;
	case MAP_GET_DO6:
		*buf = *(flash2_1 + addr);
		//gpio_get_do(addr, buf);
		break;
	case MAP_GET_DO7:
		*buf = *(flash2_1 + addr);
		//gpio_get_do(addr, buf);
		break;
	default:
		return -1;
	}
	return 0;
}

/*
 * 函数映射写入口
 * buf in 寄存器输入缓冲区
 * well_num in 井口号
 * addr in 寄存器地址
 * return {-1: 无函数映射, 0: 有函数映射}
 */
int map_map_fun_write(uint16_t *buf, int well_num, int addr) {
	Map_t *map_t = &map_map_t;
	uint16_t *flash2 = map_t->flash2 + (well_num - 1) * map_t->x_max;
	uint16_t *flash2_1 = map_t->flash2 + (1 - 1) * map_t->x_max;
	uint16_t *ram_x = map_t->ram_x + map_t->x_max * (well_num - 1);
	switch(addr) {
	case MAP_CHANGE_WELL_TYPE:
		map_item_remap(&map_map_t, well_num, *buf);
		break;
	case MAP_GET_TIME_HOUR:
		map_settime(addr, buf);
		break;
	case MAP_GET_TIME_MIN:
		map_settime(addr, buf);
		break;
	case MAP_GET_TIME_SEC:
		map_settime(addr, buf);
		break;
	case MAP_GET_TIME_YEAR:
		map_settime(addr, buf);
		break;
	case MAP_GET_TIME_MON:
		map_settime(addr, buf);
		break;
	case MAP_GET_TIME_DAY:
		map_settime(addr, buf);
		break;
	case MAP_CONFIG1:
		rtu_reset_flash1(addr, buf);
		break;
	case MAP_CONFIG2_ELEC_INTERV:
		rtu_conf_elect_interval(addr, buf);
		*(flash2 + addr) = *buf;
		*(ram_x + addr) = *buf;
		if(-1 == msync(map_t->flash2, map_t->x_max * MAX_WELL_NUM * sizeof(uint16_t), MS_SYNC)) {
			perror("flash2 msync error");
		}
		break;
	case MAP_CONFIG2_ACT_INTERV:
		rtu_conf_act_interval(addr, buf);
		*(flash2 + addr) = *buf;
		*(ram_x + addr) = *buf;
		if(-1 == msync(map_t->flash2, map_t->x_max * MAX_WELL_NUM * sizeof(uint16_t), MS_SYNC)) {
			perror("flash2 msync error");
		}
		break;
	case MAP_GET_DO0:
		gpio_set_do(addr, buf);
		*(flash2_1 + addr) = *buf;
		*(ram_x + addr) = *buf;
		if(-1 == msync(map_t->flash2, map_t->x_max * MAX_WELL_NUM * sizeof(uint16_t), MS_SYNC)) {
			perror("flash2 msync error");
		}
		break;
	case MAP_GET_DO1:
		gpio_set_do(addr, buf);
		*(flash2_1 + addr) = *buf;
		*(ram_x + addr) = *buf;
		if(-1 == msync(map_t->flash2, map_t->x_max * MAX_WELL_NUM * sizeof(uint16_t), MS_SYNC)) {
			perror("flash2 msync error");
		}
		break;
	case MAP_GET_DO2:
		gpio_set_do(addr, buf);
		*(flash2_1 + addr) = *buf;
		*(ram_x + addr) = *buf;
		if(-1 == msync(map_t->flash2, map_t->x_max * MAX_WELL_NUM * sizeof(uint16_t), MS_SYNC)) {
			perror("flash2 msync error");
		}
		break;
	case MAP_GET_DO3:
		gpio_set_do(addr, buf);
		*(flash2_1 + addr) = *buf;
		*(ram_x + addr) = *buf;
		if(-1 == msync(map_t->flash2, map_t->x_max * MAX_WELL_NUM * sizeof(uint16_t), MS_SYNC)) {
			perror("flash2 msync error");
		}
		break;
	case MAP_GET_DO4:
		gpio_set_do(addr, buf);
		*(flash2_1 + addr) = *buf;
		*(ram_x + addr) = *buf;
		if(-1 == msync(map_t->flash2, map_t->x_max * MAX_WELL_NUM * sizeof(uint16_t), MS_SYNC)) {
			perror("flash2 msync error");
		}
		break;
	case MAP_GET_DO5:
		gpio_set_do(addr, buf);
		*(flash2_1 + addr) = *buf;
		*(ram_x + addr) = *buf;
		if(-1 == msync(map_t->flash2, map_t->x_max * MAX_WELL_NUM * sizeof(uint16_t), MS_SYNC)) {
			perror("flash2 msync error");
		}
		break;
	case MAP_GET_DO6:
		gpio_set_do(addr, buf);
		*(flash2_1 + addr) = *buf;
		*(ram_x + addr) = *buf;
		if(-1 == msync(map_t->flash2, map_t->x_max * MAX_WELL_NUM * sizeof(uint16_t), MS_SYNC)) {
			perror("flash2 msync error");
		}
		break;
	case MAP_GET_DO7:
		gpio_set_do(addr, buf);
		*(flash2_1 + addr) = *buf;
		*(ram_x + addr) = *buf;
		if(-1 == msync(map_t->flash2, map_t->x_max * MAX_WELL_NUM * sizeof(uint16_t), MS_SYNC)) {
			perror("flash2 msync error");
		}
		break;
	case MAP_GET_HISTORY:
		*(ram_x + addr) = *buf;
		set_history_data(addr, buf, well_num);
		break;
	default:
		return -1;
	}
	return 0;
}

/*
 * 内部读寄存器接口
 * buf out 寄存器输出缓冲区
 * len in 寄存器的长度
 * well_num in 井口号
 * addr in 寄存器地址
 */
int zigbee_read_registers(uint16_t *buf, int len, int well_num, int addr) {
	if(well_num > MAX_WELL_NUM || well_num < 1 || addr < 0 || len < 1) {
		return -1;
	}
	Map_t *map_t = &map_map_t;
	int i = addr;
	int j = addr + len;
	if(j > map_t->x_max) {
		return -1;
	}

	char *ram_x_map_type = map_t->ram_x_map_type + map_t->x_max * (well_num - 1);
	uint16_t *ram_x = map_t->ram_x + map_t->x_max * (well_num - 1);
	uint16_t *flash1 = map_t->flash1;
	uint16_t *flash2 = map_t->flash2 + map_t->x_max * (well_num - 1);
	lock_modbus_rtu_lock(0, 0, 0);
	for(;i < j;++i) {
		switch(*(ram_x_map_type + i)) {
		case MAP_NULL:
			*buf = *(ram_x + i);
			break;
		case MAP_RAM:
			*buf = *(ram_x + i);
			break;
		case MAP_FLASH1:
			*buf = *(flash1 + i);
			break;
		case MAP_FLASH2:
			*buf = *(flash2 + i);
			break;
		case MAP_SYS:
			if(-1 == map_map_fun_read(buf, well_num, i)) {
				*buf = *(ram_x + i);
			}
			break;
		}
		++buf;
	}
	lock_modbus_rtu_unlock(0, 0, 0);
	return 0;
}

/*
 * 内部读寄存器接口(带大小端转换)
 * buf out 寄存器输出缓冲区
 * len in 寄存器的长度
 * well_num in 井口号
 * addr in 寄存器地址
 * big_l in {1: 进行大小端转换, 0: 不进行大小端转换}
 */
int zigbee_read_registers_big_l(uint16_t *buf, int len, int well_num, int addr, char big_l) {
	if(well_num > MAX_WELL_NUM || well_num < 1 || addr < 0 || len < 1) {
		return -1;
	}
	Map_t *map_t = &map_map_t;
	int i = addr;
	int j = addr + len;
	if(j > map_t->x_max) {
		return -1;
	}

	char *ram_x_map_type = map_t->ram_x_map_type + map_t->x_max * (well_num - 1);
	uint16_t *ram_x = map_t->ram_x + map_t->x_max * (well_num - 1);
	uint16_t *flash1 = map_t->flash1;
	uint16_t *flash2 = map_t->flash2 + map_t->x_max * (well_num - 1);
	uint16_t rc = -1;
	for(;i < j;++i) {
		switch(*(ram_x_map_type + i)) {
		case MAP_NULL:
			rc = *(ram_x + i);
			break;
		case MAP_RAM:
			rc = *(ram_x + i);
			break;
		case MAP_FLASH1:
			rc = *(flash1 + i);
			break;
		case MAP_FLASH2:
			rc = *(flash2 + i);
			break;
		case MAP_SYS:
			if(-1 == map_map_fun_read(&rc, well_num, i)) {
				rc = *(ram_x + i);
			}
			break;
		}
		if(big_l) {
			*(uint8_t *)(buf) = rc >> 8;
			*((uint8_t *)(buf) + 1) = rc & 0xFF;
		} else {
			*(buf) = rc;
		}
		++buf;
	}
	return 0;
}

/*
 * 清空寄存器接口
 * v in 清空值
 * len in 寄存器的长度
 * well_num in 井口号
 * addr in 寄存器地址
 */
int zigbee_write_registers_mset(uint16_t v,int len, int well_num, int addr) {
	if(well_num > MAX_WELL_NUM || well_num < 1 || addr < 0 || len < 1) {
		return -1;
	}
	Map_t *map_t = &map_map_t;
	//int i = addr;
	int j = addr + len;
	if(j > map_t->x_max) {
		return -1;
	}

	//char *ram_x_map_type = map_t->ram_x_map_type + map_t->x_max * (well_num - 1);
	uint16_t *ram_x = map_t->ram_x + map_t->x_max * (well_num - 1);
	//uint16_t *flash1 = map_t->flash1;
	//uint16_t *flash2 = map_t->flash2 + map_t->x_max * (well_num - 1);
	lock_modbus_rtu_lock(0, 0, 0);
	memset(ram_x+addr,v,len*2);
	lock_modbus_rtu_unlock(0, 0, 0);
	return 0;
}

/*
 * 内部写寄存器接口
 * buf in 寄存器输入缓冲区
 * len in 寄存器的长度
 * well_num in 井口号
 * addr in 寄存器地址
 */
int zigbee_write_registers(uint16_t *buf, int len, int well_num, int addr) {
	if(well_num > MAX_WELL_NUM || well_num < 1 || addr < 0 || len < 1) {
		return -1;
	}
	Map_t *map_t = &map_map_t;
	int i = addr;
	int j = addr + len;
	if(j > map_t->x_max) {
		return -1;
	}

	char *ram_x_map_type = map_t->ram_x_map_type + map_t->x_max * (well_num - 1);
	uint16_t *ram_x = map_t->ram_x + map_t->x_max * (well_num - 1);
	uint16_t *flash1 = map_t->flash1;
	uint16_t *flash2 = map_t->flash2 + map_t->x_max * (well_num - 1);
	lock_modbus_rtu_lock(0, 0, 0);
	for(;i < j;++i) {
		switch(*(ram_x_map_type + i)) {
		case MAP_NULL:
			*(ram_x + i) = *buf;
			break;
		case MAP_RAM:
			*(ram_x + i) = *buf;
			break;
		case MAP_FLASH1:
			*(flash1 + i) = *buf;
			if(-1 == msync(flash1, FLASH1_MAX_LEN * sizeof(uint16_t), MS_SYNC)) {
				perror("flash1 msync error");
				return -1;
			}
			*(ram_x + i) = *buf;
			break;
		case MAP_FLASH2:
			*(flash2 + i) = *buf;
			if(-1 == msync(map_t->flash2, map_t->x_max * MAX_WELL_NUM * sizeof(uint16_t), MS_SYNC)) {
				perror("flash2 msync error");
				return -1;
			}
			*(ram_x + i) = *buf;
			break;
		case MAP_SYS:
			if(-1 == map_map_fun_write(buf, well_num, i)) {
				//*(ram_x + i) = *buf;
			}
			*(ram_x + i) = *buf;
			break;
		}
		++buf;
	}
	lock_modbus_rtu_unlock(0, 0, 0);
	return 0;
}

/*
 * 外部读寄存器接口(A118 GRM, 带大小端转换)
 * buf out 寄存器输入缓冲区
 * len in 寄存器的长度
 * well_num in 井口号
 * addr in 寄存器地址
 */
int map_modbus_read_registers(uint16_t *buf, int len, int well_num, int addr, char big_l) {
	if(well_num > MAX_WELL_NUM || well_num < 1 || addr < 0 || len < 1) {
		return -1;
	}
	Map_t *map_t = &map_map_t;
	int i = addr;
	int j = addr + len;
	if(j > MAX_REGISTER_NUM) {
		return -1;
	}
	uint16_t rc = -1;
	uint16_t *ram_y = map_t->ram_y + MAX_REGISTER_NUM * (well_num - 1);
	char *ram_y_map_type = map_t->ram_y_map_type + MAX_REGISTER_NUM * (well_num - 1);

	for(;i < j;++i) {
		switch(*(ram_y_map_type + i)) {
		case MAP_NULL:
			rc = *(ram_y + i);
			break;
		case MAP_RAM:
			zigbee_read_registers(&rc, 1, well_num, *(ram_y + i));
			break;
		}
		if(big_l) {
			*(uint8_t *)(buf) = rc >> 8;
			*((uint8_t *)(buf) + 1) = rc & 0xFF;
		} else {
			*(buf) = rc;
		}
		++buf;
	}
	return 0;
}

/*
 * 外部写寄存器接口(A118 GRM, 带大小端转换)
 * buf in 寄存器输入缓冲区
 * len in 寄存器的长度
 * well_num in 井口号
 * addr in 寄存器地址
 */
int map_modbus_write_registers(uint16_t *buf, int len, int well_num, int addr, char big_l) {
	printf("%d %d %d\n", well_num, addr, len);
	if(well_num > MAX_WELL_NUM || well_num < 1 || addr < 0 || len < 1) {
		return -1;
	}
	Map_t *map_t = &map_map_t;
	int i = addr;
	int j = addr + len;
	printf("%d %d\n", i, j);
	if(j > MAX_REGISTER_NUM) {
		return -1;
	}

	uint16_t *ram_y = map_t->ram_y + MAX_REGISTER_NUM * (well_num - 1);
	char *ram_y_map_type = map_t->ram_y_map_type + MAX_REGISTER_NUM * (well_num - 1);
	char *ram_x_map_rw = map_t->ram_x_map_rw + map_t->x_max * (well_num - 1);

	uint16_t rc = -1;
	for(;i < j;++i) {
		if(big_l) {
			rc = ((*(uint8_t *)(buf)) << 8) + (*((uint8_t *)(buf) + 1));
		} else {
			rc = *buf;
		}
		switch(*(ram_y_map_type + i)) {
		case MAP_NULL:
			*(ram_y + i) = rc;
			break;
		case MAP_RAM:
			if(MAP_RW != *(ram_x_map_rw + *(ram_y + i))) {
				printf("MAP_RAM %d\n", *(ram_x_map_rw + *(ram_y + i)));
				continue;
			}
			zigbee_write_registers(&rc, 1, well_num, *(ram_y + i));
			break;
		}
		++buf;
	}
	return 0;
}

int map_modbus_write_data_registers(uint16_t *buf, int len, int well_num, int addr, char big_l) {
	printf("%d %d %d\n", well_num, addr, len);
	if(well_num > MAX_WELL_NUM || well_num < 1 || addr < 0 || len < 1) {
		return -1;
	}
	Map_t *map_t = &map_map_t;
	int i = addr;
	int j = addr + len;
	printf("%d %d\n", i, j);
	if(j > MAX_REGISTER_NUM) {
		return -1;
	}

	uint16_t *ram_y = map_t->ram_y + MAX_REGISTER_NUM * (well_num - 1);
	char *ram_x_record = map_t->ram_x_map_record + map_t->x_max * (well_num - 1);
	uint16_t k = 0;
	uint16_t m = 0;
	uint16_t rc = -1;
	for(;i < j;++i, ++k) {
		if(big_l) {
			rc = ((*(uint8_t *)(buf)) << 8) + (*((uint8_t *)(buf) + 1));
		} else {
			rc = *buf;
		}
		if(*(ram_x_record + k) == MAP_RECORD) {
			*(ram_y + m + addr) = rc;
			//printf("%d %d\n", k, m + addr);
			++m;
		}
		++buf;
	}
	return 0;
}


/* 写寄存器后的动作(未使用) */
int map_modbus_after_write_registers(uint16_t *buf, int len, int well_num, int addr, char big_l) {
	printf("%d %d %d\n", well_num, addr, len);
	if(well_num > MAX_WELL_NUM || well_num < 1 || addr < 0 || len < 1) {
		return -1;
	}
	Map_t *map_t = &map_map_t;
	int i = addr;
	int j = addr + len;
	printf("%d %d\n", i, j);
	if(j > MAX_REGISTER_NUM) {
		return -1;
	}

	uint16_t *ram_y = map_t->ram_y + MAX_REGISTER_NUM * (well_num - 1);
	char *ram_y_map_type = map_t->ram_y_map_type + MAX_REGISTER_NUM * (well_num - 1);
	char *ram_x_map_rw = map_t->ram_x_map_rw + map_t->x_max * (well_num - 1);

	uint16_t rc = -1;
	for(;i < j;++i) {
		if(big_l) {
			rc = ((*(uint8_t *)(buf)) << 8) + (*((uint8_t *)(buf) + 1));
		} else {
			rc = *buf;
		}
		switch(*(ram_y_map_type + i)) {
		case MAP_NULL:
			*(ram_y + i) = rc;
			break;
		case MAP_RAM:
			if(MAP_RW != *(ram_x_map_rw + *(ram_y + i))) {
				printf("MAP_RAM %d\n", *(ram_x_map_rw + *(ram_y + i)));
				continue;
			}
			zigbee_write_registers(&rc, 1, well_num, *(ram_y + i));
			break;
		}
		++buf;
	}
	return 0;
}


#if 0
int main() {
	if(-1 == map_init(&map_map_t)) {
		perror("map_init error");
		return -1;
	}
	uint16_t buf[1024];
	int i;
	int j;
	int n = 1024;

//	buf[0] = 5;
//	buf[1] = 5;
//	map_modbus_write_registers(buf, 1, 1, 1, 0);

//	zigbee_write_registers(buf, 1, 1, 1);

//	for(i = 0;i < MAX_WELL_NUM;++i) {
//		zigbee_read_registers(buf, n, i + 1, 0);
//		for(j = 0;j < n;++j) {
//			printf("%hd,", buf[j]);
//		}
//		printf("\n\n");
//	}

	map_item_remap(&map_map_t, 1, 3);

	for(i = 0;i < 1;++i) {
		map_modbus_read_registers(buf, n, i + 1, 0, 0);
		for(j = 0;j < n;++j) {
			printf("%hd,", buf[j]);
		}
		printf("\n\n");
	}
//
//	buf[0] = 3;
//	buf[1] = 2;
//	zigbee_write_registers(buf, 2, 1, 0);
//	printf("ram_map_flash1_value_init ok\n");
	return 0;
}
#endif
