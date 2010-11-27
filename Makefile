obj-m += tcp_flow_spy.o

all:
		make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
		make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

reinstall:
		sudo /sbin/rmmod tcp_flow_spy
		sudo /sbin/insmod tcp_flow_spy.ko number_of_buckets=10 bucket_length=5 bufsize=100 live=1 
	
install:
		sudo /sbin/insmod tcp_flow_spy.ko number_of_buckets=10 bucket_length=5 bufsize=100 live=1
