# Garbage collector for Starfish

Based on ['Boehm-Demers-Weiser Garbage Collector'](/bdwgc/README.md).

# Terms
### Roots
Generally `roots` mean pointers stored in statically allocated or stack allocated program variables.  
A mark-sweep garbage collector traverses all reachable objects in the heap (more accurately it's gc heap in our case) by following pointers beginning with the `roots`.  
In this project `roots` include followings,
* (Add here)
* (Add here)

### Garbage-collected heap
(TODO)

### (Add term here)
...

# Coding guide

### Interaction with the system malloc
It is usually best not to mix garbage-collected allocation with the system malloc-free. If you do, you need to be careful not to store pointers to the garbage-collected heap in memory allocated with the system malloc.


### Using std::vector (1)
You need to be careful using std::vector since it allocates memory for its elements internally.  
It allocates based on the allocator you pass in at construction. If you didn't specify one, you get the default allocator and it will allocate on the native heap!  
So it is needed to specify GC allocator for the std::vector with elements that may have GC pointers.  

To elaborate, at mark phase, GC start to scan pointer-like values and checks if the values belong to the GC heap. But if we allocate on the native heap using std allocator, the ranges are out of GC heap range, so GC ignores the values.

### Using std::vector (2)
It is not recommended to use std::vector, because its `std::vector::end` points next space of the last element which makes GC think it as a non-collectable space!
Please refer [here](http://en.cppreference.com/w/cpp/container/vector/end).


### (Add here)