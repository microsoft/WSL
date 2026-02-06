// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include "TestTasks.h"

using namespace wsl::windows::common::wslutil;
using namespace wsl::windows::wslc::execution;

namespace wsl::windows::wslc::task
{
    void OutputNinjaCat(CLIExecutionContext& context)
    {
        if (context.Args.Contains(ArgType::TestArg))
        {
            std::wostringstream testout;
            static constexpr std::wstring_view s_ninjaCat = LR"(
                                    -<vYT`                                           
                                 ')hM3d$$c                                           
                              `~uydNv>>$$$^                                          
                        -T}uymkZdqUdN3mowkh_                                         
                         T06m\xwslcclixYxcky`                                        
                          wDNf3omhXucVi!"YyjL                                        
                          `VmhkuvrHG}VVT)~:_:!           r!`-_                       
                           `hqf-.-TH3r"`     !-`r^,`-!:^)kUdE"                       
                            -jGhkyY*-        'T!YyTykzGNkxLxv-                       
,]Vyx!`                                      `"kKPGHKzhHy)*>'                        
  `_|fZV*-                           `*\*~~*LuVkXzXUm3GGx|x`      `<}cc*`            
     _ryGNm]~-                        `_"!^**vL}ycVkh5MG}|L:    !V0QMv=r|_``         
       `"rumdNbfyLr>!:",_--_""",---_!*iwHNNNZxv}XqbNNNNNNNNZX3RQBQQR98QTrrr*.        
          `"*ThZNNNNNNNNNNNNNNNNNN9NNNNNNNNNN]VNNNbNNNNdNNNQ#BBBB80N8#@QZc}}^`       
             `">vuhMNNNNNNNNNNN6$gENNNNNNR9&NKyZNNNRgd53GbN0#BBB$N60bNRNNNZ3XT!      
                 .:*]coPbNNNNNN$Q$NNNdNNN$QQ6NNNNN6QR5Xy3bNEdMMENNNbkT5NNNNN60T      
                    `-:^)YuyX3Z0QNNdMMZdN8QBQRNNN68Q6Z55dbMqqqGqdNNNUy#Ho5NNG3y      
                         `._=*vc0NZGKa5dEBBBBQQ8QQQ8NNNgGjjjjjx=:yNNNqgBN}`          
                               ~NZ3kyhq9B03oycuuymdNNN$ZvvxcqU~`  :zN}Q#8:           
                               }bHk}cmZNcxxxxxxxYVhg9N3!   .V3ZEm, .MZkZ@v           
                             `rPMKyVhG*-````-L8@@@@@@#x}3yvr:`  r&^ 'cddoX.          
                            xd#N53Um9r         m####BM   rdGY.  :PV:::,:-      __`   
                           ^NN#BZ5EQv         -QBQ$N3'   `=^.!>;,.`  `-,"::",:r!,~   
                         ~q68#@@@Dx`          rQ86Nd"           .=;~-        `x--    
                        `P9BBbx:`             ~9NNX-                _~;!' -^)*:,"    
                        rN6QZ`               `KNNZ_                    `"x*' -^      
                        yNgD5^`              XNNNy                       >)``        
                       .NNg#NQa`             ^qN$N}.                     `:r^        
                        ZNk#QNQQdx-           !N@#RgQy,-                             
                       _Zy ,h03o= `           -!G@BNQ@#85T.                          
                        `      `                :*vxL`)L! '                          
)";
            testout << std::endl << s_ninjaCat << std::endl;
            PrintMessage(testout.str(), stdout);
        }
    }
}
