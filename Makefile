
SRC=\
energygraph.c \
grapher.c


energygraph: $(SRC)
	$(CC) -g -Wall -o energygraph $(SRC) -lm

clean:
	rm -f energygraph *.o

run: energygraph
	sudo ./energygraph

