import typer
import shlex
import pabo as pb
import numpy as np
from rich import print
from pathlib import Path
import scipy.sparse as sp
from subprocess import run
from rich.table import Table
from rich.panel import Panel


def dispersive_delay(
    f: float,
    f0: float,
    dm: float,
) -> float:
    return 4.1488064239e3 * dm * (f**-2 - f0**-2)


def fprint(x: dict) -> None:
    grid = Table.grid(expand=True, padding=(0, 2, 0, 2))
    grid.add_column(justify="left", style="bold")
    grid.add_column(justify="right", style="italic")
    for key, value in x.items():
        grid.add_row(str(key), str(value))
    print(
        Panel(
            grid,
            padding=2,
            expand=False,
        )
    )


def main(
    nf: int,
    fh: float,
    bw: float,
    dt: float,
    dm: float,
    tsys: float,
    gain: float,
    flux: float,
    width: float,
    tburst: float,
    outfile: Path,
    spectrum: str = "",
    dm_index: float = -2,
):
    df = bw / nf
    fl = fh - (df * nf) + 0.5 * df
    tmax = round(dispersive_delay(fl, fh, dm))

    with open(f"sys_{outfile.stem}.params", "w+") as f:
        f.write(
            "name: FAKE FRB #1000\n"
            "telescope: GMRT\n"
            "observer: upanda\n"
            f"f1: {fl}\n"
            f"f2: {fh}\n"
            f"nchan: {nf}\n"
            f"t0: 0.0\n"
            f"t1: {tmax}\n"
            f"tsamp: {dt}\n"
            f"gain: {gain}\n"
            f"tsys: {tsys}\n"
            "nbits: 2\n"
        )

    with open(f"{outfile.stem}.params", "w+") as f:
        f.write(f"dmburst: 0.0 {fh} {flux} {dm_index} {width} {dm} 2" + f" {spectrum}")

    run(
        shlex.split(
            f"simulateBurst -p sys_{outfile.stem}.params -p {outfile.stem}.params -o {outfile.stem}.dat"
        )
    )

    with open(f"{outfile.stem}.dat", "rb") as f:
        _raj_rad = None
        _decj_rad = None
        _writelabels = 0

        _fmt = pb.CString().parse(f.read(64))
        match _fmt:
            case "FORMAT 1.0":
                _compressed = False
                _name = pb.PaddedString(128).parse(f)
                _t1 = pb.Float(4).parse(f)
                _t2 = pb.Float(4).parse(f)
                _dt = pb.Float(4).parse(f)
                _f1 = pb.Float(4).parse(f)
                _f2 = pb.Float(4).parse(f)
                _nf = pb.Int(4).parse(f)
                _raj_rad = pb.Float(4).parse(f)
                _decj_rad = pb.Float(4).parse(f)
                _useangle = pb.Int(4).parse(f)
                _initialseed = pb.Int(8).parse(f)
            case "FORMAT 1.1":
                _compressed = False
                _name = pb.PaddedString(128).parse(f)
                _t1 = pb.Float(4).parse(f)
                _t2 = pb.Float(4).parse(f)
                _dt = pb.Float(4).parse(f)
                _f1 = pb.Float(4).parse(f)
                _f2 = pb.Float(4).parse(f)
                _nf = pb.Int(4).parse(f)
                _raj_rad = pb.Float(4).parse(f)
                _decj_rad = pb.Float(4).parse(f)
                _useangle = pb.Int(4).parse(f)
                _initialseed = pb.Int(8).parse(f)
                _writelabels = pb.Int(4).parse(f)
                if _writelabels == 1:
                    raise NotImplementedError("No support for labels yet.")
            case "FORMAT 1.2":
                _compressed = False
                _name = pb.PaddedString(128).parse(f)
                _t1 = pb.Float(4).parse(f)
                _t2 = pb.Float(4).parse(f)
                _dt = pb.Float(4).parse(f)
                _f1 = pb.Float(4).parse(f)
                _f2 = pb.Float(4).parse(f)
                _nf = pb.Int(4).parse(f)
                _postype = pb.Int(4).parse(f)
                if _postype == 1:
                    _raj_rad = pb.Float(4).parse(f)
                    _decj_rad = pb.Float(4).parse(f)
                else:
                    _posfname = pb.PaddedString(128).parse(f)
                _useangle = pb.Int(4).parse(f)
                _initialseed = pb.Int(8).parse(f)
                _writelabels = pb.Int(4).parse(f)
                if _writelabels == 1:
                    raise NotImplementedError("No support for labels yet.")
            case "FORMAT 2.1":
                _compressed = True
                _name = pb.PaddedString(128).parse(f)
                _t1 = pb.Float(4).parse(f)
                _t2 = pb.Float(4).parse(f)
                _dt = pb.Float(4).parse(f)
                _f1 = pb.Float(4).parse(f)
                _f2 = pb.Float(4).parse(f)
                _nf = pb.Int(4).parse(f)
                _postype = pb.Int(4).parse(f)
                if _postype == 1:
                    _raj_rad = pb.Float(4).parse(f)
                    _decj_rad = pb.Float(4).parse(f)
                else:
                    _posfname = pb.PaddedString(128).parse(f)
                _useangle = pb.Int(4).parse(f)
                _initialseed = pb.Int(8).parse(f)
                _writelabels = pb.Int(4).parse(f)
                if _writelabels == 1:
                    raise NotImplementedError("No support for labels yet.")
            case _:
                raise NotImplementedError("This format not supported.")
        fprint(
            dict(
                name=_name,
                t1=_t1,
                t2=_t2,
                dt=_dt,
                f1=_f1,
                f2=_f2,
                nf=_nf,
                raj_rad=_raj_rad,
                decj_rad=_decj_rad,
                compressed=_compressed,
                useangle=_useangle != 0,
                initialseed=_initialseed,
                writelabels=_writelabels != 0,
            )
        )
        data = np.fromfile(f, dtype=np.float32)
    transformed = sp.coo_array(np.fliplr(data.reshape(-1, nf)))
    with open(outfile, "wb+") as f:
        pb.Spec(
            dict(
                nf=pb.Int(8),
                nt=pb.Int(8),
                nnz=pb.Int(8),
                dm=pb.Float(8),
                flux=pb.Float(8),
                width=pb.Float(8),
                tburst=pb.Float(8),
            )
        ).build(
            dict(
                dm=dm,
                flux=flux,
                width=width,
                tburst=tburst,
                nnz=transformed.nnz,
                nt=transformed.shape[0],
                nf=transformed.shape[1],
            ),
            f,
        )
        transformed.row.tofile(f)
        transformed.col.tofile(f)
        transformed.data.tofile(f)


if __name__ == "__main__":
    typer.run(main)
