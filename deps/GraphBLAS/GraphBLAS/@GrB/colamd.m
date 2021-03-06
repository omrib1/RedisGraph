function [p, varargout] = colamd (G, varargin)
%COLAMD column approximate minimum degree ordering of a GraphBLAS matrix.
% See 'help colamd' for details.

% SuiteSparse:GraphBLAS, Timothy A. Davis, (c) 2017-2019, All Rights Reserved.
% http://suitesparse.com   See GraphBLAS/Doc/License.txt for license.

[p, varargout{1:nargout-1}] = colamd (double (G), varargin {:}) ;

