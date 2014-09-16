#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <omp.h>

#include "common.h"
#include "timer.h"

namespace {

struct Option
{
    Option() 
        : eta(0.1f), lambda(0.00002f), iter(15), nr_factor(4), 
          nr_factor_real(4), nr_threads(1), do_prediction(true) {}
    std::string Tr_path, Va_path;
    float eta, lambda;
    uint32_t iter, nr_factor, nr_factor_real, nr_threads;
    bool do_prediction;
};

std::string train_help()
{
    return std::string(
"usage: mark33 [<options>] <train_path>\n"
"\n"
"options:\n"
"-l <lambda>: you know\n"
"-k <dimension>: you know\n"
"-t <iteration>: you know\n"
"-r <eta>: you know\n"
"-s <nr_threads>: you know\n"
"-q: you know\n");
}

Option parse_option(std::vector<std::string> const &args)
{
    uint32_t const argc = static_cast<uint32_t>(args.size());

    if(argc == 0)
        throw std::invalid_argument(train_help());

    Option opt; 

    uint32_t i = 0;
    for(; i < argc; ++i)
    {
        if(args[i].compare("-t") == 0)
        {
            if(i == argc-1)
                throw std::invalid_argument("invalid command");
            opt.iter = std::stoi(args[++i]);
        }
        else if(args[i].compare("-k") == 0)
        {
            if(i == argc-1)
                throw std::invalid_argument("invalid command");
            opt.nr_factor_real = std::stoi(args[++i]);
            opt.nr_factor = static_cast<uint32_t>(ceil(static_cast<float>(opt.nr_factor_real)/4.0f))*4;
        }
        else if(args[i].compare("-r") == 0)
        {
            if(i == argc-1)
                throw std::invalid_argument("invalid command");
            opt.eta = std::stof(args[++i]);
        }
        else if(args[i].compare("-l") == 0)
        {
            if(i == argc-1)
                throw std::invalid_argument("invalid command");
            opt.lambda = std::stof(args[++i]);
        }
        else if(args[i].compare("-s") == 0)
        {
            if(i == argc-1)
                throw std::invalid_argument("invalid command");
            opt.nr_threads = std::stoi(args[++i]);
        }
        else if(args[i].compare("-q") == 0)
        {
            opt.do_prediction = false;
        }
        else
        {
            break;
        }
    }

    if(i >= argc-1)
        throw std::invalid_argument("training data not specified");

    opt.Va_path = args[i++];
    opt.Tr_path = args[i++];

    return opt;
}

void init_model(Model &model, uint32_t const nr_factor_real)
{
    uint32_t const nr_factor = model.nr_factor;
    float const coef = 
        static_cast<float>(0.5/sqrt(static_cast<double>(nr_factor_real)));

    float * w = model.W.data();
    for(uint32_t j = 0; j < model.nr_feature; ++j)
    {
        for(uint32_t f = 0; f < model.nr_field; ++f)
        {
            for(uint32_t d = 0; d < nr_factor_real; ++d, ++w)
                *w = coef*static_cast<float>(drand48());
            for(uint32_t d = nr_factor_real; d < nr_factor; ++d, ++w)
                *w = 0;
            for(uint32_t d = nr_factor; d < 2*nr_factor; ++d, ++w)
                *w = 1;
        }
    }
}

void train(SpMat const &Tr, SpMat const &Va, Model &model, Option const &opt)
{
    std::vector<uint32_t> order(Tr.Y.size());
    for(uint32_t i = 0; i < Tr.Y.size(); ++i)
        order[i] = i;

    Timer timer;
    for(uint32_t iter = 0; iter < opt.iter; ++iter)
    {
        timer.tic();

        double Tr_loss = 0;
        std::random_shuffle(order.begin(), order.end());
#pragma omp parallel for schedule(static)
        for(uint32_t i_ = 0; i_ < order.size(); ++i_)
        {
            uint32_t const i = order[i_];

            float const y = Tr.Y[i];
            
            float const t = wTx(Tr, model, i);

            float const expnyt = static_cast<float>(exp(-y*t));

            Tr_loss += log(1+expnyt);
               
            float const kappa = -y*expnyt/(1+expnyt);

            wTx(Tr, model, i, kappa, opt.eta, opt.lambda, true);
        }

        printf("%3d %8.2f %10.5f", iter, timer.toc(), 
            Tr_loss/static_cast<double>(Tr.Y.size()));

        if(Va.Y.size() != 0)
            printf(" %10.5f", predict(Va, model));

        printf("\n");
        fflush(stdout);
    }
}

void scan(SpMat const &Tr, Model &model)
{
    for(uint32_t i = 0; i < Tr.nr_instance; ++i)
    {
        for(uint32_t f1 = 0; f1 < Tr.nr_field; ++f1)
        {
            uint32_t const j1 = Tr.J[i*Tr.nr_field+f1];
            if(j1 >= model.nr_feature)
                continue;

            for(uint32_t f2 = f1+1; f2 < Tr.nr_field; ++f2)
            {
                uint32_t const j2 = Tr.J[i*Tr.nr_field+f2];
                if(j2 >= model.nr_feature)
                    continue;

                uint32_t w_idx = calc_w_idx(j1, j2);

                ++model.WP2[w_idx].mask;
            }
        }
    }

    uint32_t const threshold = 
        static_cast<uint32_t>(static_cast<double>(Tr.nr_instance)*0.01);
    for(auto &w : model.WP2)
        w.mask = (w.mask <= threshold)? 0 : 1;
}

} //unnamed namespace

int main(int const argc, char const * const * const argv)
{
    Option opt;
    try
    {
        opt = parse_option(argv_to_args(argc, argv));
    }
    catch(std::invalid_argument const &e)
    {
        std::cout << "\n" << e.what() << "\n";
        return EXIT_FAILURE;
    }

    printf("reading data...");
    fflush(stdout);
    SpMat const Va = read_data(opt.Va_path);
    printf("Va...");
    fflush(stdout);
    SpMat const Tr = read_data(opt.Tr_path);
    printf("done\n");
    fflush(stdout);

    printf("initializing model...");
    fflush(stdout);
    Model model(Tr.nr_feature, opt.nr_factor, Tr.nr_field);

    init_model(model, opt.nr_factor_real);
    printf("done\n");
    fflush(stdout);

    printf("scanning...");
    scan(Tr, model);
    printf("done\n");
    fflush(stdout);

	omp_set_num_threads(static_cast<int>(opt.nr_threads));

    train(Tr, Va, model, opt);

	omp_set_num_threads(1);

    if(opt.do_prediction)
    {
        predict(Tr, model, opt.Tr_path+".out");
        predict(Va, model, opt.Va_path+".out");
    }

    return EXIT_SUCCESS;
}
