int fib(int n) {
	if(n < 2)
		return n;
	return fib(n - 1) + fib(n - 2);
}

int counter;
int initialized = 7;
char *message = "jit stress ok";

int main(int argc, char **argv) {
	int i;
	int total = 0;
	counter = 0;
	for(i = 0; i < 10; i++)
		counter += i;
	total = fib(15);
	printf("counter=%v total=%v initialized=%v message=%v\n",
		counter, total, initialized, message);
	printf("argc=%v argv0=%v\n", argc, argv[0]);
	if(counter == 45 && total == 610)
		return 42;
	return 1;
}
