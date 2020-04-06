# Copyright (C) Jan 2020 Mellanox Technologies Ltd. All rights reserved.   
#                                                                           
# This software is available to you under a choice of one of two            
# licenses.  You may choose to be licensed under the terms of the GNU       
# General Public License (GPL) Version 2, available from the file           
# COPYING in the main directory of this source tree, or the                 
# OpenIB.org BSD license below:                                             
#                                                                           
#     Redistribution and use in source and binary forms, with or            
#     without modification, are permitted provided that the following       
#     conditions are met:                                                   
#                                                                           
#      - Redistributions of source code must retain the above               
#        copyright notice, this list of conditions and the following        
#        disclaimer.                                                        
#                                                                           
#      - Redistributions in binary form must reproduce the above            
#        copyright notice, this list of conditions and the following        
#        disclaimer in the documentation and/or other materials             
#        provided with the distribution.                                    
#                                                                           
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,         
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF        
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND                     
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS       
# BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN        
# ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN         
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE          
# SOFTWARE.                                                                 
# --                                                                        


#######################################################
#
# TerminateSegment.py
# Python implementation of the Class TerminateSegment
# Generated by Enterprise Architect
# Created on:      14-Aug-2019 10:11:57 AM
# Original author: talve
#
#######################################################
from segments.Segment import Segment
from segments.SegmentFactory import SegmentFactory
from utils import constants as cs


class TerminateSegment(Segment):
    """this class is responsible for holding Terminate segment data.
    """
    def __init__(self, data):
        """initialize the class by setting the class data.
        """
        self.raw_data = data
        self._parsed_data = {}

    def get_data(self):
        """get the general segment data.
        """
        return self.raw_data

    def get_type(self):
        """get the general segment type.
        """
        return cs.RESOURCE_DUMP_SEGMENT_TYPE_TERMINATE

    def get_parsed_data(self):
        """get dictionary of parsed segment data.
        """
        #parsed_data = {'Segment Type': "Terminate Segment" + " ({0})".format(cs.RESOURCE_DUMP_SEGMENT_TYPE_TERMINATE)}
        return self._parsed_data


SegmentFactory.register(cs.RESOURCE_DUMP_SEGMENT_TYPE_TERMINATE, TerminateSegment)
