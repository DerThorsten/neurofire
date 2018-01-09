from numbers import Integral
import numpy as np

import torch
import torch.nn as nn

from .loss_transforms import MaskTransitionToIgnoreLabel


class BalanceAffinities(object):
    """
    Compute a weight for different classes, based on the distribution
    """
    def __init__(self, ignore_label=None, offsets=None):
        # if we have an ignore label, we need to instantiate the masking
        # function (which masks all ignore labels and transitions to the ignore label)
        if ignore_label is not None:
            assert isinstance(ignore_label, Integral)
            assert offsets is not None
            self.masker = MaskTransitionToIgnoreLabel(offsets, ignore_label)
        self.ignore_label = ignore_label

    def __call__(self, prediction, target):
        scales = torch.ones_like(prediction)
        # if we have an ignore label, compute and apply the mask
        if self.ignore_label is not None:
            assert target.size(1) - 1 == prediction.size(1)
            segmentation = target[:, 0:1]
            mask = self.masker.full_mask_tensor(segmentation)
            scales *= mask
            target_affinities = target[:, 1:]
        else:
            assert target.size(1) == prediction.size(1)
            target_affinities = target
        # compute the number of labeled samples and the
        # fraction of positive / negative samples
        n_labeled = scales.sum()
        frac_positive = (scales * target_affinities).sum() / n_labeled
        frac_positive = np.clip(frac_positive.data[0], 0.05, 0.95)
        frac_negative = 1. - frac_positive
        # compte the corresponding class weights
        # (this is done as in
        # https://github.com/funkey/gunpowder/blob/master/gunpowder/nodes/balance_affinity_labels.py#L47
        # I don't understand exactly why to choose this as weighting)
        w_positive = 1. / (2. * frac_positive)
        w_negative = 1. / (2. * frac_negative)
        return torch.from_numpy(np.array([w_negative, w_positive]))


class LossWrapper(nn.Module):
    """
    Wrapper around a torch criterion.
    Enables transforms before applying the criterion.
    Should be subclassed for implementation.
    """
    def __init__(self,
                 criterion,
                 transforms=None,
                 weight_function=None):
        super(LossWrapper, self).__init__()
        # validate: the criterion needs to inherit from nn.Module
        assert isinstance(criterion, nn.Module)
        self.criterion = criterion
        # validate: transforms need to be callable
        if transforms is not None:
            assert callable(transforms)
        self.transforms = transforms
        if weight_function is not None:
            assert callable(weight_function)
        self.weight_function = weight_function

    def forward(self, prediction, target):
        # calculate the weight based on prediction and target
        if self.weight_function is not None:
            weight = self.weight_function(prediction, target)
            self.criterion.weight = weight

        if self.transforms is not None:
            transformed_prediction, transformed_target = self.transforms(prediction, target)
        else:
            transformed_prediction, transformed_target = prediction, target

        loss = self.criterion(transformed_prediction, transformed_target)
        return loss


# TODO something analogous to `AsSegmentationCriterion` from neuro-skunkworks to
# move loss preprocessing to the gpu ?!