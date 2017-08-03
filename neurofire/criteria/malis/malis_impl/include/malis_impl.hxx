#pragma once

#include <algorithm>
#include <map>

#include "nifty/marray/marray.hxx"
#include "nifty/tools/for_each_coordinate.hxx"
#include "nifty/ufd/ufd.hxx"

namespace malis_impl{

// TODO implement different nhoods
template<unsigned DIM, typename DATA_TYPE, typename LABEL_TYPE>
void compute_malis_gradient(
    const nifty::marray::View<DATA_TYPE> & affinities,
    const nifty::marray::View<LABEL_TYPE> & groundtruth,
    const bool pos,
    nifty::marray::View<DATA_TYPE> & gradientsOut,
    DATA_TYPE & lossOut,
    DATA_TYPE & classificationErrorOut,
    DATA_TYPE & randIndexOut
) {

    typedef nifty::array::StaticArray<int64_t,DIM>   Coord;
    typedef nifty::array::StaticArray<int64_t,DIM+1> AffinityCoord;
    typedef LABEL_TYPE LabelType;
    typedef DATA_TYPE DataType;

    // check that number of affinity channels matches the dimensions
    NIFTY_CHECK_OP(affinities.shape(0),==,DIM,"Number of affinity channels does not match the dimension!");
    NIFTY_CHECK_OP(gradientsOut.shape(0),==,DIM,"Number of gradient channels must match !");
    // check that shapes match
    for(int d = 0; d < DIM; ++d) {
        NIFTY_CHECK_OP(affinities.shape(d+1),==,groundtruth.shape(d),"Affinity shape does not match gt shape!");
        NIFTY_CHECK_OP(affinities.shape(d+1),==,gradientsOut.shape(d+1),"Affinity shape does not match gradients shape!");
    }

    // 1.) Initialize the union-find and the overlap vector, which stores for every pixel
    // the overlap with ground-truth segments in a map
    // also count the gt segment sizes

    // number of nodes and edges in the affinity graph
    const int numberOfNodes = groundtruth.size(); // the number of node corresponds to the number of pixels / voxels
    const int numberOfEdges = affinities.size();  // the number of edges corresponds to the number of affinities

    Coord pixelShape;
    for(int d = 0; d < DIM; ++d) {
        pixelShape[d] = groundtruth.shape(d);
    }

    // union find, overlaps and segment sizes
    nifty::ufd::Ufd<LabelType> sets(numberOfNodes);
    std::vector<std::map<LabelType, size_t>> overlaps(numberOfNodes);
    std::map<LabelType, size_t> segmentSizes;

    // number of positive pairs and labeled nodes for normalization
    size_t numberOfLabeledNodes = 0, nPairPos = 0;

    size_t nodeIndex = 0;
    nifty::tools::forEachCoordinate(pixelShape, [&](Coord coord) {
        auto gtId = groundtruth(coord.asStdArray());

        if(gtId != 0) {
            overlaps[nodeIndex].insert(std::make_pair(gtId, 1));
            ++segmentSizes[gtId];
            ++numberOfLabeledNodes;
            nPairPos += (segmentSizes[gtId] - 1);
        }

        ++nodeIndex;
    });

    // compute normalizations
    size_t nPairNorm;
    if (pos) {
        nPairNorm = nPairPos;
    }
    else {
        size_t nPairTot = (numberOfLabeledNodes * (numberOfLabeledNodes - 1)) / 2;
        nPairNorm = nPairTot - nPairPos;
    }
    if(nPairNorm <= 0) {
        throw std::runtime_error("Normalization is zero or negative!");
    }

    // 2.) Sort all affinity edges in increasing order of weight

    AffinityCoord affinityShape;
    for(int d = 0; d < DIM+1; ++d) {
        affinityShape[d] = affinities.shape(d);
    }

    // get a flattened view to the marray
    size_t flatShape[] = {affinities.size()};
    auto flatView = affinities.reshapedView(flatShape, flatShape+1);

    // initialize the pqueu as [0,1,2,3,...,numberOfEdges]
    std::vector<size_t> pqueue(numberOfEdges);
    std::iota(pqueue.begin(), pqueue.end(), 0);

    // sort pqueue in increasing order
    std::sort(pqueue.begin(), pqueue.end(), [&flatView](const size_t ind1, const size_t ind2){
        return (flatView(ind1) > flatView(ind2));
    });

    // 3.) Run kruskal - for each min spanning tree edge,
    // we compute the loss and gradient

    size_t axis;
    LabelType setU, setV, nodeU, nodeV;
    size_t nPair = 0, nPairIncorrect = 0 ;
    double loss = 0, gradient = 0;
    Coord gtCoordU, gtCoordV;
    AffinityCoord affCoord;
    typename std::map<LabelType,size_t>::iterator itU, itV;
    DATA_TYPE affinity;

    // iterate over the pqueue
    for(auto edgeIndex : pqueue) {

        // translate edge index to coordinate
        affCoord[0] = edgeIndex / affinities.strides(0);
        for(int d = 1; d < DIM+1; ++d) {
            affCoord[d] = (edgeIndex % affinities.strides(d-1) ) / affinities.strides(d);
        }

        // the axis this edge is refering to
        axis = affCoord[0];

        // first, we copy the spatial coordinates of the affinity pixel for both gt coords
        for(int d = 1; d < DIM + 1; ++d) {
            gtCoordU[d - 1] = affCoord[d];
            gtCoordV[d - 1] = affCoord[d];
        }

        // convention: edges encode the affinty to higher coordinates
        //if(gtCoordV[axis] < pixelShape[axis] - 1) {
        //    ++gtCoordV[axis];
        //}

        // convention: edges encode the affinty to lower coordinates
        if(gtCoordV[axis] > 0) {
            --gtCoordV[axis];
        }
        else {
            continue;
        }


        nodeU = 0;
        nodeV = 0;
        for(int d = 0; d < DIM; ++d) {
            nodeU += gtCoordU[d] * groundtruth.strides(d);
            nodeV += gtCoordV[d] * groundtruth.strides(d);
        }
        setU = sets.find(nodeU);
        setV = sets.find(nodeV);

        // only do stuff if the two segments are not merged yet
        if(setU != setV) {

            //
            // debug out
            //

            //std::cout << "Edge: " << edgeIndex << std::endl;
            //std::cout << "Corresponding to affinity coordinate: ";
            //for(int d = 0; d < DIM + 1; ++d) {
            //    std::cout << affCoord[d] << " ";
            //}
            //std::cout << std::endl;

            // std::cout << "GT coordinates U: ";
            // for(int d = 0; d < DIM; ++d) {
            //     std::cout << gtCoordU[d] << " ";
            // }
            // std::cout << std::endl;

            // std::cout << "GT coordinates V: ";
            // for(int d = 0; d < DIM; ++d) {
            //     std::cout << gtCoordV[d] << " ";
            // }
            // std::cout << std::endl;

            // std::cout << "Corresponding to nodes: " << nodeU << " " << nodeV << std::endl;
            // std::cout << "Correspodning to Sets: " << setU << " " << setV << std::endl;

            sets.merge(setU, setV);

            // compute the number of pairs merged by this edge
            for (itU = overlaps[setU].begin(); itU != overlaps[setU].end(); ++itU) {
                for (itV = overlaps[setV].begin(); itV != overlaps[setV].end(); ++itV) {

                    // the number of pairs that are joind by this edge are given by the
                    // number of pix associated with U times pix associated with V
                    nPair = itU->second * itV->second;

                    // for pos:
                    // we add nPairs if we join two nodes in the same gt segment
                    if (pos && (itU->first == itV->first)) {

                        // std::cout << "Adding pos loss for " << nPair << " pairs" << std::endl;

                        affinity = affinities(affCoord.asStdArray());
                        gradient = 1. - affinity;
                        loss += gradient * gradient * nPair;
                        gradientsOut(affCoord.asStdArray()) += gradient * nPair;

                        // if the affinity for this edge is smaller than 0.5, although the two nodes are connected in the
                        // groundtruth, this is a classification error
                        if(affinity <= .5) {
                            nPairIncorrect += nPair;
                        }
                    }

                    // for !pos:
                    // we add nPairs if we join two nodes in different gt segments
                    else if (!pos && (itU->first != itV->first)) {

                        // std::cout << "Adding neg loss for " << nPair << " pairs" << std::endl;

                        affinity = affinities(affCoord.asStdArray());
                        gradient = -affinity;
                        loss += gradient * gradient * nPair;
                        gradientsOut(affCoord.asStdArray()) += gradient * nPair;

                        // if the affinity for this edge is bigger than 0.5, although the two nodes are not connected in the
                        // groundtruth, this is a classification error
                        if(affinity > .5) {
                            nPairIncorrect += nPair;
                        }
                    }
                }
            }

            // normalize the gradients
            gradientsOut(affCoord.asStdArray()) /= nPairNorm;

            // move the pixel bags of the non-representative to the representative
            if (sets.find(setU) == setV) // make setU the rep to keep and setV the rep to empty
                std::swap(setU,setV);

            itV = overlaps[setV].begin();
            while (itV != overlaps[setV].end()) {
                itU = overlaps[setU].find(itV->first);
                if (itU == overlaps[setU].end()) {
                    overlaps[setU].insert( std::make_pair(itV->first,itV->second) );
                }
                else {
                    itU->second += itV->second;
                }
                overlaps[setV].erase(itV++);
            }
        }
    }

    // 4.) Outputs

    // return the loss
    if (nPairNorm > 0) {
        loss /= nPairNorm;
    }
    else {
        loss = 0;
    }
    lossOut = loss;

    // return the classification error and rand index
    classificationErrorOut = static_cast<DATA_TYPE>(nPairIncorrect) / static_cast<DATA_TYPE>(nPairNorm);
    randIndexOut = 1. - static_cast<DATA_TYPE>(nPairIncorrect) / static_cast<DATA_TYPE>(nPairNorm);

}


template<unsigned DIM, typename DATA_TYPE, typename LABEL_TYPE>
void compute_constrained_malis_gradient(
    const nifty::marray::View<DATA_TYPE> & affinities,
    const nifty::marray::View<LABEL_TYPE> & groundtruth,
    nifty::marray::View<DATA_TYPE> & gradientsOut,
    DATA_TYPE & lossOut
) {

    typedef nifty::array::StaticArray<int64_t,DIM>   Coord;
    typedef nifty::array::StaticArray<int64_t,DIM+1> AffinityCoord;
    typedef LABEL_TYPE LabelType;
    typedef DATA_TYPE DataType;

    AffinityCoord affShape;
    for(int d = 0; d < DIM + 1; ++d) {
        affShape[d] = affinities.shape(d);
    }

    Coord pixelShape;
    for(int d = 0; d < DIM; ++d) {
        pixelShape[d] = groundtruth.shape(d);
    }

    // construct the affinities for negative and positive pass on the fly
    // we don't need to initialize here, this is done below
    nifty::marray::Marray<DataType> affinitiesPos(nifty::marray::SkipInitialization, affShape.begin(), affShape.end());
    nifty::marray::Marray<DataType> affinitiesNeg(nifty::marray::SkipInitialization, affShape.begin(), affShape.end());

    size_t axis;
    Coord gtCoordU, gtCoordV;
    //AffinityCoord affCoord;
    LabelType labelU, labelV;
    DataType affinity;

    // we iterate over all the edges, constructing the affinity values for positive and negative pass
    // the positive affinities are set to min(affinities, gtAffinities)
    // the negative affinities are set to max(affinities, gtAffinities)
    // gtAffinities are 0 for invalid edges, edges with different node labels or edges which have an ignore label in their associated node labels
    // they are 1 otherwise (same node label, which is not ignore or invalid edge)
    // this can be simplified a bit by only calculating the gtAffinity implcitly and setting
    // affintyPos = 0., affinityNeg = affinity if gtAffinity would be 0
    // affintyPos = affinity, affinityNeg = 1. if gtAffinity would be 1
    nifty::tools::forEachCoordinate(affShape, [&](AffinityCoord affCoord) {

        affinity = affinities(affCoord.asStdArray());

        // we change the V coordinate for the given axis (=corresponding coordinate)
        // only if this results in a valid coordinate
        axis = affCoord[0];

        // first, we copy the spatial coordinates of the affinity pixel for both gt coords
        for(int d = 1; d < DIM + 1; ++d) {
            gtCoordU[d - 1] = affCoord[d];
            gtCoordV[d - 1] = affCoord[d];
        }

        // convention: edges encode the affinty to higher coordinates
        //if(gtCoordV[axis] < pixelShape[axis] - 1) {
        //    ++gtCoordV[axis];
        //}

        // convention: edges encode the affinty to lower coordinates
        if(gtCoordV[axis] > 0) {
            --gtCoordV[axis];
        }
        else {
            // the edge is invalid -> gtAffinity would be 0
            affinitiesPos(affCoord.asStdArray()) = 0.;
            affinitiesNeg(affCoord.asStdArray()) = affinity;
            return;
        }


        labelU = groundtruth(gtCoordU.asStdArray());
        labelV = groundtruth(gtCoordV.asStdArray());
        if(labelU != labelV || labelU == 0 || labelV == 0) {
            // gtAffinity would be 0
            affinitiesPos(affCoord.asStdArray()) = 0.;
            affinitiesNeg(affCoord.asStdArray()) = affinity;
        }
        else {
            // gtAffinity would be 1
            affinitiesPos(affCoord.asStdArray()) = affinity;
            affinitiesNeg(affCoord.asStdArray()) = 1.;
        }

    });


    // FIXME in-place adding of gradients yields different result for some reason I can't really explain...

    //// TODO if we want to weight pos and neg differently, we need to pass a corresponding factor
    //// to compute malis loss
    //// calculate the gradients
    //// note that gradients are added in-place to gradientsOut

    //// calculate the positive malis gradients
    //DataType lossPos, rand, classErr;
    //compute_malis_gradient<DIM>(
    //    affinitiesPos, groundtruth, true, gradientsOut, lossPos, classErr, rand
    //);

    //// calculate the negative malis gradients
    //DataType lossNeg;
    //compute_malis_gradient<DIM>(
    //    affinitiesNeg, groundtruth, false, gradientsOut, lossNeg, classErr, rand
    //);

    //lossOut = (lossPos + lossNeg) / 2.;


    nifty::marray::Marray<DataType> gradNeg(affShape.begin(), affShape.end(), 0);
    nifty::marray::Marray<DataType> gradPos(affShape.begin(), affShape.end(), 0);

    // TODO if we want to weight pos and neg differently, we need to pass a corresponding factor
    // to compute malis loss
    // calculate the gradients
    // note that gradients are added in-place to gradientsOut

    // calculate the positive malis gradients
    DataType lossPos, rand, classErr;
    compute_malis_gradient<DIM>(
        affinitiesPos, groundtruth, true, gradPos, lossPos, classErr, rand
    );

    // calculate the negative malis gradients
    DataType lossNeg;
    compute_malis_gradient<DIM>(
        affinitiesNeg, groundtruth, false, gradNeg, lossNeg, classErr, rand
    );

    gradientsOut = gradNeg + gradPos;
    lossOut = (lossPos + lossNeg) / 2.;
}

}
