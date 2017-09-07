#include "glow/Importer/Caffe2.h"
#include "glow/Network/Image.h"
#include "glow/Network/Network.h"
#include "glow/Network/Nodes.h"
#include "glow/Network/Tensor.h"

using namespace glow;

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "Usage:  " << argv[0] << " network_structure.pb weights.pb\n";
    return -1;
  }

  auto *data = new Tensor(ElemKind::FloatTy, {8, 224, 224, 3});
  auto *expected_softmax = new Tensor(ElemKind::IndexTy, {8, 1});

  auto H = data->getHandle<FloatTy>();
  H.randomize(1);

  glow::Network N;
  caffe2ModelLoader LD(argv[1], argv[2], {"data", "softmax_expected"},
                       {data, expected_softmax}, N);

  auto *SM = LD.getNodeByName("prob");

  N.dumpGraph();

  auto *res = N.infer(SM, {}, {});
  res->getHandle<FloatTy>().dump();
  return 0;
}