![logo](mobject_logo.png) 
# Mobject

[![check spelling](https://github.com/hyoklee/mobject/actions/workflows/spell.yml/badge.svg)](https://github.com/hyoklee/mobject/actions/workflows/spell.yml)
[![ior+mobject@develop](https://github.com/hyoklee/mobject/actions/workflows/ior.yml/badge.svg)](https://github.com/hyoklee/mobject/actions/workflows/ior.yml)
[![spack mobject](https://github.com/hyoklee/mobject/actions/workflows/spack.yml/badge.svg)](https://github.com/hyoklee/mobject/actions/workflows/spack.yml)
[![spack mobject+bedrock](https://github.com/hyoklee/mobject/actions/workflows/spack_bedrock.yml/badge.svg)](https://github.com/hyoklee/mobject/actions/workflows/spack_bedrock.yml)

Mobject is a distributed object storage system
built using a composition of [Mochi](https://mochi.readthedocs.io) components: 
 
 * [mochi-bake](https://github.com/mochi-hpc/mochi-bake) (for bulk storage)
 * [mochi-bedrock](https://github.com/mochi-hpc/mochi-bedrock) 
   (for configuration and bootstrapping) 
 * [mochi-yokan](https://github.com/mochi-hpc/mochi-yokan)
   (for metadata and log indexing)
 * [mochi-ssg](https://github.com/mochi-hpc/mochi-ssg) (for group membership)

 
## Installation

  [Install Spack and Mochi Spack Repository](https://mochi.readthedocs.io/en/latest/installing.html#installing-spack-and-the-mochi-repository).
  
  Then, run the following command to install mobject.
```
   spack install mobject
```

## HDF5 and Mobject

  [Mobject API](/include/librados-mobject-store.h) is a subset of the 
  [RADOS API](https://github.com/ceph/ceph/blob/main/src/include/rados/librados.h) 
  from Ceph’s object storage layer.
Therefore, [HDF5 RADOS VOL plugin-in](https://github.com/HDFGroup/vol-rados)
can use Mobject.

## FAQ

See [doc/FAQ.md](doc/FAQ.md).


