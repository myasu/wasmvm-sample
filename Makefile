# コンパイラ
CC = gcc

# コンパイルオプション
CFLAGS = -Wall -O2 -g

# 出力する実行ファイル名
TARGET = test

# ソースコード
SRCS = main.c

# オブジェクトファイル
OBJS = $(SRCS:.c=.o)

# ルール
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(OBJS) $(TARGET)

dump: $(TARGET)
	objdump -dS test > objdump.txt
