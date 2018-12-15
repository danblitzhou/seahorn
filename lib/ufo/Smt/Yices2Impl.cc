#ifdef WITH_YICES2

#include <gmp.h>
#include "yices.h"

#include "ufo/Smt/Yices2Impl.hpp"
#include "ufo/Smt/Yices2ModelImpl.hpp"

using namespace expr;


namespace seahorn {
  namespace yices {

    static std::map<expr::Expr, term_t> s_cache;


    /* flag to indicate library status; we are single threaded so we can be lazy. */
    static bool s_yices_lib_initialized = false;


    inline void yices_library_initialize(void){
      if( !s_yices_lib_initialized ){
        s_yices_lib_initialized = true;
        yices_init();
      }
    }




    /* how should we set the default logic? */
    yices_impl::yices_impl(std::string logic, seahorn::solver::solver_options *opts, expr::ExprFactory &efac):
      d_efac(efac)
    {
        yices_library_initialize();
        /* the yices configuration structure */
        ctx_config_t *cfg;

        if ( logic.empty() && !opts ){
          cfg = NULL;
        } else {
          cfg = yices_new_config();
          if ( ! logic.empty() ){
            int32_t errcode = yices_default_config_for_logic(cfg, logic.c_str());
          }
          if ( opts ){
            /* iterate through the opts map and set the keys */

          }

        }

        d_ctx = yices_new_context(cfg);
        yices_free_config(cfg);

    }

    yices_impl::~yices_impl(){
      yices_free_context(d_ctx);
    }





    bool yices_impl::add(expr::Expr exp){

      return false;
    }

    /** Check for satisfiability */
    solver::Solver::result yices_impl::check(){

      return solver::Solver::UNKNOWN;
    }

    /** Push a context */
    void yices_impl::push(){
      yices_push(d_ctx);
    }

    /** Pop a context */
    void yices_impl::pop(){
      yices_push(d_ctx);
    }

    /** Get a model */
    solver::model* yices_impl::get_model(){
      model_t *model = yices_get_model(d_ctx, 1); //BD & JN: keep subst??
      return new model_impl(model, s_cache, d_efac);
    }



  }

}





#endif
