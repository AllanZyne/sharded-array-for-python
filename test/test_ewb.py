import ddptensor as dt
import numpy
from utils import runAndCompare, mpi_dtypes, on_gpu
import pytest

mpi_idtypes = [  # FIXME
    # dt.int64,
    # dt.uint64,
    dt.int32,
    dt.uint32,
    # dt.int8,
    # dt.uint8,
]


class TestEWB:
    def test_add0(self):
        for dtyp in mpi_dtypes:
            a = dt.ones((6, 6), dtype=dtyp)
            c = a + a
            r1 = dt.sum(c)
            v = 6 * 6 * 2
            assert float(r1) == v

    def test_add1(self):
        for dtyp in mpi_dtypes:
            a = dt.ones((6, 6), dtype=dtyp)
            b = dt.ones((6, 6), dtype=dtyp)
            c = a + b
            r1 = dt.sum(c)
            v = 6 * 6 * 2
            assert float(r1) == v

    def test_add2(self):
        for dtyp in mpi_idtypes:
            a = dt.ones((16, 16), dtype=dtyp)
            c = a + 1
            r1 = dt.sum(c, [0, 1])
            v = 16 * 16 * 2
            assert float(r1) == v

    def test_add3(self):
        for dtyp in mpi_idtypes:
            a = dt.ones((16, 16), dtype=dtyp)
            b = dt.ones((16, 16), dtype=dtyp)
            c = a + b + 1
            r1 = dt.sum(c, [0, 1])
            v = 16 * 16 * 3
            assert float(r1) == v

    def test_add4(self):
        for dtyp in mpi_idtypes:
            n = 16
            a = dt.fromfunction(lambda i, j: i, (n, n), dtype=dtyp)
            b = dt.ones((n, n), dtyp)
            c = a + b
            a[:, :] = a[:, :] + c[:, :]
            r1 = dt.sum(a, [0, 1])
            v = n * n * n
            assert float(r1) == v

    def test_add_mul(self):
        def doit(aapi):
            a = aapi.zeros((16, 16), dtype=aapi.int64)
            b = aapi.ones((12, 12), dtype=aapi.int64)
            a[3:13, 3:13] = b[0:10, 1:11] + b[1:11, 1:11] * b[1, 1]
            return a

        assert runAndCompare(doit)

    def test_add_shifted1(self):
        for dtyp in mpi_idtypes:
            aa = dt.ones((16, 16), dtype=dtyp)
            bb = dt.ones((16, 16), dtype=dtyp)
            a = aa[0:8, 0:16]
            b = bb[5:13, 0:16]
            c = a + b + 1
            r1 = dt.sum(c, [0, 1])
            v = 8 * 16 * 3
            assert float(r1) == v

    @pytest.mark.skip(reason="FIXME reshape")
    def test_add_shifted2(self):
        def doit(aapi):
            a = aapi.reshape(aapi.arange(0, 64, 1, dtype=aapi.float32), [8, 8])
            b = aapi.reshape(aapi.arange(0, 64, 1, dtype=aapi.float32), [8, 8])
            c = a[2:6, 0:8]
            d = b[0:8:2, 0:8]
            return c + d

        assert runAndCompare(doit)

    def test_add_shifted3(self):
        def doit():
            aa = dt.arange(0, 64, 1, dtype=dt.int32)
            bb = dt.arange(0, 64, 1, dtype=dt.int32)
            a = aa[0:8]
            b = bb[50:58]
            c = a + b + 1
            return dt.sum(c)

        r1 = doit()
        assert int(r1) == 464

    def test_add_shifted4(self):
        aa = dt.arange(0, 64, 1, dtype=dt.int32)
        a = aa[0:8]
        b = aa[50:58]
        c = a + b + 1
        r1 = dt.sum(c)
        assert int(r1) == 464

    def test_add_shifted5(self):
        aa = dt.arange(0, 64, 1, dtype=dt.int32)
        bb = dt.arange(0, 64, 1, dtype=dt.int32)
        a = aa[0:16:2]
        b = bb[40:64:3]
        c = a + b + 1
        r1 = dt.sum(c)
        assert int(r1) == 468

    def test_add_shifted6(self):
        aa = dt.arange(0, 64, 1, dtype=dt.int32)
        a = aa[0:16:2]
        b = aa[30:54:3]
        c = a + b + 1
        r1 = dt.sum(c)
        assert int(r1) == 388

    def test_add_broadcast(self):
        def doit(aapi):
            a = aapi.zeros((16, 16), dtype=aapi.int32)
            b = aapi.arange(1, 16, 1, dtype=aapi.int32)
            a[3:13, 3:13] = a[0:10, 1:11] + b[0]
            return a

        assert runAndCompare(doit)

    @pytest.mark.skip(reason="FIXME")
    def test_prod_het(self):
        a = dt.full([16, 16], 2, dt.float32)
        b = dt.full([16, 16], 2, dt.int32)
        c = a * b
        r = dt.sum(c, [0, 1])
        v = 16 * 16 * 2 * 2
        assert float(r) == v

    @pytest.mark.skipif(on_gpu, reason="FIXME power on GPU")
    def test_pow(self):
        for dtyp in [dt.int32, dt.float32]:
            a = dt.full((6, 6), 3, dtype=dtyp)
            b = dt.full((6, 6), 2, dtype=dtyp)
            c = a**b
            r1 = dt.sum(c, [0, 1])
            v = 6 * 6 * 9
            assert float(r1) == v

    def test_bitwise_and(self):
        for dtyp in mpi_idtypes:
            a = dt.full((6, 6), 3, dtype=dtyp)
            b = dt.full((6, 6), 2, dtype=dtyp)
            c = a & b
            r1 = dt.sum(c, [0, 1])
            v = 6 * 6 * 2
            assert float(r1) == v

    def test_add_typecast(self):
        type_list = [
            (dt.int8, dt.int64, dt.int64),
            (dt.int64, dt.int8, dt.int64),
            (dt.int8, dt.float64, dt.float64),
            (dt.float64, dt.int8, dt.float64),
            (dt.float32, dt.float64, dt.float64),
            (dt.float32, dt.int64, dt.float64),
            (dt.int32, dt.uint32, dt.int64),
            (dt.int32, dt.uint64, dt.int64),
        ]
        for atype, btype, ctype in type_list:
            a = dt.arange(0, 8, 1, dtype=atype)
            b = dt.ones((8,), dtype=btype)
            c = a + b
            assert c.dtype == ctype
            c2 = dt.to_numpy(c)
            assert numpy.allclose(c2, [1, 2, 3, 4, 5, 6, 7, 8])

    def test_reflected_sub(self):
        a = dt.full((4,), 4, dtype=dt.float64)
        b = 10.0
        c = b - a
        c2 = dt.to_numpy(c)
        assert numpy.allclose(c2, [6, 6, 6, 6])

    def test_reflected_div(self):
        a = dt.full((4,), 2, dtype=dt.float64)
        b = 10.0
        c = b / a
        c2 = dt.to_numpy(c)
        assert numpy.allclose(c2, [5, 5, 5, 5])
