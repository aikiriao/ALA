CC 		    = gcc
CFLAGS 	  = -std=c89 -Wall -Wextra -Wpedantic -Wformat=2 -Wconversion -O0 -g3
CPPFLAGS	= -DNDEBUG
LDFLAGS		= -Wall -Wextra -Wpedantic
LDLIBS		= -lm
OBJS	 		= main.o wav.o bit_stream.o ala_coder.o ala_predictor.o ala_utility.o 
TARGET    = alac

all: $(TARGET) 

rebuild:
	make clean
	make all

clean:
	rm -f $(OBJS) $(TEST_OBJS) $(TARGET)

$(TARGET) : $(OBJS) $(TEST_OBJS)
	$(CC) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $(TARGET)

.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $<
