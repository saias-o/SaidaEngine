"""The road network: the single source of truth for the city's layout.

Everything downstream derives from the grid built here — which kit tile is laid
at each cell and how it is turned, where the sidewalks and blocks are, and the
lane graph the traffic drives on. The city therefore cannot drift out of
agreement with its own traffic.

Directions are the four cardinals in tile space, with +X east and +Z south:

    N = (0, -1)   S = (0, +1)   E = (+1, 0)   W = (-1, 0)
"""

N, S, E, W = (0, -1), (0, 1), (1, 0), (-1, 0)
CARDINALS = (N, S, E, W)

# The city is 50 tiles square. At TILE = 8 m that is 400 m on a side.
GRID = 50

# Avenues run north-south at these column indices, streets run east-west at
# these row indices. The spacing is deliberately uneven so the blocks are not
# all the same size.
AVENUES = (4, 11, 18, 25, 32, 39, 46)
STREETS = (4, 10, 16, 22, 28, 34, 40)
BOULEVARD = 43          # the seafront street, south of the last block row
BEACH_ROW = 45          # sand starts here and runs to the water

# How a kit tile is authored, as the set of edges a road continues through.
# Read out of the meshes themselves: a tile edge closed by a full-length kerb is
# a pavement, an edge with only two short corner returns is an opening.
PIECES = (
    ("road-crossroad",    frozenset((W, E, N, S))),
    ("road-intersection", frozenset((W, E, S))),
    ("road-straight",     frozenset((W, E))),
    ("road-bend",         frozenset((W, S))),
    ("road-end",          frozenset((E,))),
)


def rotate(d, yaw):
    """Turn a direction by a yaw in degrees, the same way the engine turns a node.

    A rotation about +Y maps (x, z) to (x cos + z sin, -x sin + z cos), so a yaw
    of 90 degrees takes east to north.
    """
    x, z = d
    if yaw == 0:
        return (x, z)
    if yaw == 90:
        return (z, -x)
    if yaw == 180:
        return (-x, -z)
    if yaw == 270:
        return (-z, x)
    raise ValueError("unsupported yaw %r" % (yaw,))


def piece_for(openings):
    """The kit tile and yaw whose openings match, or None if nothing fits.

    Searching for the rotation rather than hard-coding it means a wrong reading
    of how a tile is authored fails loudly here instead of quietly producing a
    city whose roads do not join up.
    """
    want = frozenset(openings)
    for model, default in PIECES:
        for yaw in (0, 90, 180, 270):
            if frozenset(rotate(d, yaw) for d in default) == want:
                return model, yaw
    return None


class RoadNetwork:
    """The set of road cells, their connections, and what to lay on each."""

    def __init__(self):
        self.cells = set()
        self._build()

    # ── layout ──────────────────────────────────────────────────────────────
    def _build(self):
        first_street, last_street = STREETS[0], STREETS[-1]
        first_av, last_av = AVENUES[0], AVENUES[-1]

        for i in AVENUES:                       # north-south avenues
            for j in range(first_street, BOULEVARD + 1):
                self.cells.add((i, j))
        for j in STREETS:                       # east-west streets
            for i in range(first_av, last_av + 1):
                self.cells.add((i, j))
        for i in range(first_av, last_av + 1):  # the seafront boulevard
            self.cells.add((i, BOULEVARD))

    # ── queries ─────────────────────────────────────────────────────────────
    def openings(self, cell):
        i, j = cell
        return frozenset(d for d in CARDINALS if (i + d[0], j + d[1]) in self.cells)

    def tiles(self):
        """Yield (cell, model, yaw) for every road cell."""
        for cell in sorted(self.cells):
            found = piece_for(self.openings(cell))
            if found is None:
                raise AssertionError(
                    "no road tile matches the openings at %r: %r"
                    % (cell, sorted(self.openings(cell))))
            yield cell, found[0], found[1]

    def is_junction(self, cell):
        return len(self.openings(cell)) >= 3

    def blocks(self):
        """The rectangles enclosed by the grid, as (i0, i1, j0, j1) inclusive
        tile ranges of the block interior."""
        out = []
        for a in range(len(AVENUES) - 1):
            for s in range(len(STREETS) - 1):
                i0, i1 = AVENUES[a] + 1, AVENUES[a + 1] - 1
                j0, j1 = STREETS[s] + 1, STREETS[s + 1] - 1
                if i1 >= i0 and j1 >= j0:
                    out.append((i0, i1, j0, j1))
        # The strip between the last street and the seafront boulevard.
        for a in range(len(AVENUES) - 1):
            i0, i1 = AVENUES[a] + 1, AVENUES[a + 1] - 1
            j0, j1 = STREETS[-1] + 1, BOULEVARD - 1
            if i1 >= i0 and j1 >= j0:
                out.append((i0, i1, j0, j1))
        return out

    # ── the lane graph the traffic system drives on ─────────────────────────
    def lane_graph(self, to_world):
        """Centreline graph: one node per road cell, one edge per connection.

        Lanes are not baked with a lateral offset here. The centreline is what
        stays correct through a junction, so the traffic agent carries the
        offset and the graph stays a plain adjacency the pathfinder can use.
        """
        index = {cell: n for n, cell in enumerate(sorted(self.cells))}
        nodes = []
        for cell in sorted(self.cells):
            x, z = to_world(*cell)
            nodes.append({
                "id": index[cell],
                "pos": [round(x, 3), round(z, 3)],
                "degree": len(self.openings(cell)),
            })
        edges = []
        for cell in sorted(self.cells):
            i, j = cell
            for d in self.openings(cell):
                other = (i + d[0], j + d[1])
                if index[cell] < index[other]:      # emit each pair once
                    edges.append([index[cell], index[other]])
        return {"nodes": nodes, "edges": edges}
