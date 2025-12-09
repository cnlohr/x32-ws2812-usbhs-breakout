all : testtop

testtop : testtop.c
	gcc -g -o $@ $^ -I../../../minichlink -ludev -lusb-1.0

install_udev_rules : 
	sudo cp 99-x32-ws2812-usbhs-breakout.rules /etc/udev/rules.d/
	sudo udevadm control --reload
	sudo udevadm trigger

clean :
	rm -rf *.o *~ testtop

