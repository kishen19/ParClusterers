import sys
import csv
import time
import networkit as nk

def readGraph(filename):
  print_index = 1000000
  index_track = 0
  nodes = set()
  edges_from = []
  edges_to = []
  weights = []
  with open(filename, "r") as in_file:
    for line in in_file:
      if index_track % print_index == 0:
        print("We're at: " + str(index_track))
        sys.stdout.flush()
      index_track = index_track + 1
      line = line.strip()
      if not line:
        continue
      if line[0] == '#':
        continue
      split = [x.strip() for x in line.split('\t')]
      if split:
        a = split[0]
        b = split[1]
        w = 1
        if len(split) == 3:
          w = split[2]
          weights.append(float(w))
          weights.append(float(w))
        nodes.add(int(a))
        nodes.add(int(b))
        edges_from.append(int(a))
        edges_to.append(int(b))
        edges_from.append(int(b))
        edges_to.append(int(a))
        
  return nodes, edges_from, edges_to, weights

# TODO: add it for weighted graphs
def NKGraphToEdgeLists(G):
  edge_list = [(u, v) for u, v in G.iterEdges()]

  print("read " + str(len(edge_list)) + " edges")

  nodes = set()
  edges_from = []
  edges_to = []
  weights = []

  for (u,v) in edge_list:
    nodes.add(u)
    nodes.add(v)
    edges_from.append(u)
    edges_to.append(v)
    edges_from.append(v)
    edges_to.append(u)

  
  return nodes, edges_from, edges_to, weights

def readNKGraphToEdgeLists(filename):
  start_time = time.time()
  G = nk.readGraph(filename, nk.Format.NetworkitBinary)
  end_time = time.time()
  print("Read Graph in %f \n" % (end_time - start_time))
  return NKGraphToEdgeLists(G)

def convert_to_tigergraph_format(filename, input_dir, output_dir, use_nk = False):

  if use_nk:
    nodes, edges_from, edges_to, weights = readNKGraphToEdgeLists(input_dir + filename)
  else:
    nodes, edges_from, edges_to, weights = readGraph(input_dir + filename)
    

  node_list = list(nodes)
  with open(output_dir + filename + '_nodes.csv', 'w') as f:
      
      writer = csv.writer(f)
      writer.writerow(['Number', 'ID'])

      for i in range(len(node_list)):
        writer.writerow([i, node_list[i]])

  with open(output_dir + filename + '_edges.csv', 'w') as f:
      
      writer = csv.writer(f)
      if len(weights) == 0:
        writer.writerow(['Number', 'Node1', 'Node2'])

        for i in range(0, len(edges_to), 2):
          writer.writerow([i/2, edges_to[i], edges_to[i+1]])
      else: 
        writer.writerow(['Number', 'Node1', 'Node2', 'Weight'])

        for i in range(0, len(edges_to), 2):
          writer.writerow([i/2, edges_to[i], edges_to[i+1], weights[int(i/2)]])

def main():
  args = sys.argv[1:]
  convert_to_tigergraph_format(args[0], args[1], args[2])

if __name__ == "__main__":
  main()